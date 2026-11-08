/**
 * @file cert_store.c
 * @brief CertStore implementation. See cert_store.h and docs/lld/middleware/cert-store.md.
 */

#include "cert_store.h"

#include <string.h>

#include "config_store/config_store_crc.h"
#include "mbedtls/sha256.h"

#ifdef TEST
#define CERT_STORE_RAM /* host builds: ordinary static storage */
#else
#define CERT_STORE_RAM                                                                             \
    __attribute__((section(".bss.sram2"))) /**< the 96 KB bank is nearly full                      \
                                            */
#endif

#define SECTOR_SIZE (4096UL)
#define SLOT_SIZE (2UL * SECTOR_SIZE) /**< two erase sectors per slot */
#define SLOT_COUNT (2U)
#define MAGIC (0x54524543UL) /**< "CERT" */
#define FORMAT_VERSION (2U)
#define PAGE_SIZE (256UL)
#define SHA256_BYTES (32U)
#define KEY_SALT "envmon-cert-store-v1"
#define DER_SEQUENCE (0x30U)
#define DER_LONG_FORM (0x80U)
#define DER_LEN_BYTES_MASK (0x7FU)

typedef struct
{
    uint32_t magic;
    uint32_t sequence;
    uint32_t crc; /**< over the body as stored (pass and key encrypted) */
    uint16_t version;
    uint16_t reserved; /**< zero */
} slot_header_t;

typedef struct
{
    uint8_t ssid_len;
    uint8_t pass_len;
    uint16_t ca_len;
    uint16_t client_len;
    uint16_t key_len;
    uint8_t endpoint_len;
    uint8_t reserved; /**< zero */
    char endpoint[CERT_STORE_ENDPOINT_MAX + 1U];
    char ssid[CERT_STORE_SSID_MAX + 1U];
    char pass[CERT_STORE_PASS_MAX + 1U]; /**< encrypted on flash */
    uint8_t ca[CERT_STORE_CERT_MAX];
    uint8_t client[CERT_STORE_CERT_MAX];
    uint8_t key[CERT_STORE_KEY_MAX]; /**< encrypted on flash */
} slot_body_t;

_Static_assert(sizeof(slot_header_t) == 16U, "slot header");
_Static_assert((sizeof(slot_header_t) + sizeof(slot_body_t)) <= SLOT_SIZE, "a slot is two sectors");

static slot_body_t s_body CERT_STORE_RAM; /* the decrypted copy in use */
static const iqspi_flash_t *s_flash;
static uint8_t s_uid[CERT_STORE_UID_BYTES];
static uint32_t s_seq;
static uint32_t s_active; /* slot holding the copy in use */
static bool s_inited;

/* ------------------------------------------------------------------------- */
/* Encryption of the secret fields                                            */
/* ------------------------------------------------------------------------- */

/* Stream cipher from SHA-256 in counter mode: block j = SHA256(key || sequence || field || j), key
 * = SHA256(salt || unique ID). The sequence changes on every write, so a keystream is never used
 * twice. Symmetric: the same call encrypts and decrypts. The key is bound to the chip, not secret:
 * reading the flash of another device (or by a probe without the ID) does not reveal the fields; a
 * local attacker with code execution on this chip can derive the key (the limit is recorded in the
 * LLD). */
static void crypt_field(uint8_t *data, uint32_t len, uint32_t sequence, uint8_t field)
{
    uint8_t seed[sizeof(KEY_SALT) - 1U + CERT_STORE_UID_BYTES];
    uint8_t key[SHA256_BYTES];
    uint8_t blockin[SHA256_BYTES + 9U];
    uint8_t block[SHA256_BYTES];

    (void) memcpy(seed, KEY_SALT, sizeof(KEY_SALT) - 1U);
    (void) memcpy(&seed[sizeof(KEY_SALT) - 1U], s_uid, CERT_STORE_UID_BYTES);
    (void) mbedtls_sha256(seed, sizeof(seed), key, 0);

    (void) memcpy(blockin, key, SHA256_BYTES);
    (void) memcpy(&blockin[SHA256_BYTES], &sequence, 4U);
    blockin[SHA256_BYTES + 4U] = field;
    for (uint32_t off = 0U; off < len; off += SHA256_BYTES)
    {
        const uint32_t counter = off / SHA256_BYTES;
        (void) memcpy(&blockin[SHA256_BYTES + 5U], &counter, 4U);
        (void) mbedtls_sha256(blockin, sizeof(blockin), block, 0);
        for (uint32_t i = 0U; (i < SHA256_BYTES) && ((off + i) < len); i++)
        {
            data[off + i] ^= block[i];
        }
    }
    (void) memset(key, 0, sizeof(key));
    (void) memset(block, 0, sizeof(block));
}

static void crypt_secrets(uint32_t sequence)
{
    crypt_field((uint8_t *) s_body.pass, sizeof(s_body.pass), sequence, 1U);
    crypt_field(s_body.key, sizeof(s_body.key), sequence, 2U);
}

/* ------------------------------------------------------------------------- */
/* Flash                                                                       */
/* ------------------------------------------------------------------------- */

static uint32_t slot_addr(uint32_t slot)
{
    return CERT_STORE_BASE + (slot * SLOT_SIZE);
}

static uint32_t body_crc(const slot_body_t *body)
{
    uint32_t c = config_store_crc32_begin();

    c = config_store_crc32_feed(c, (const uint8_t *) body, (uint32_t) sizeof(*body));
    return config_store_crc32_end(c);
}

/* A slot is valid when its header is right and the CRC of the body, read in pieces, matches. The
 * body is not copied here: the candidate would cost another 6.8 KB of SRAM2. */
static bool slot_valid(uint32_t slot, slot_header_t *hdr)
{
    uint8_t piece[PAGE_SIZE];

    if ((s_flash->read(slot_addr(slot), (uint8_t *) hdr, (uint32_t) sizeof(*hdr)) !=
         QSPI_FLASH_OK) ||
        (hdr->magic != MAGIC) || (hdr->version != FORMAT_VERSION))
    {
        return false;
    }
    uint32_t crc = config_store_crc32_begin();

    for (uint32_t off = 0U; off < sizeof(slot_body_t); off += PAGE_SIZE)
    {
        const uint32_t n =
            ((sizeof(slot_body_t) - off) < PAGE_SIZE) ? (sizeof(slot_body_t) - off) : PAGE_SIZE;

        if (s_flash->read(slot_addr(slot) + sizeof(*hdr) + off, piece, n) != QSPI_FLASH_OK)
        {
            return false;
        }
        crc = config_store_crc32_feed(crc, piece, n);
    }
    return config_store_crc32_end(crc) == hdr->crc;
}

static bool program(uint32_t addr, const uint8_t *data, uint32_t len)
{
    while (len > 0U)
    {
        const uint32_t room = PAGE_SIZE - (addr % PAGE_SIZE);
        const uint32_t n = (len < room) ? len : room;

        if (s_flash->write_page(addr, data, (uint16_t) n) != QSPI_FLASH_OK)
        {
            return false;
        }
        addr += n;
        data += n;
        len -= n;
    }
    return true;
}

/* Write the RAM copy to the slot that does not hold the copy in use: body first, header last. */
static cert_store_err_t save(void)
{
    const uint32_t target = (s_active + 1U) % SLOT_COUNT;
    const uint32_t seq = s_seq + 1U;
    slot_header_t hdr = {
        .magic = MAGIC, .sequence = seq, .version = FORMAT_VERSION, .reserved = 0U};

    crypt_secrets(
        seq); /* the RAM copy is encrypted in place for the write and decrypted after it */
    hdr.crc = body_crc(&s_body);

    const bool ok = (s_flash->erase_sector(slot_addr(target)) == QSPI_FLASH_OK) &&
                    (s_flash->erase_sector(slot_addr(target) + SECTOR_SIZE) == QSPI_FLASH_OK) &&
                    program(slot_addr(target) + sizeof(hdr), (const uint8_t *) &s_body,
                            (uint32_t) sizeof(s_body)) &&
                    program(slot_addr(target), (const uint8_t *) &hdr, (uint32_t) sizeof(hdr));

    crypt_secrets(seq);
    if (!ok)
    {
        return CERT_STORE_ERR_FLASH;
    }
    s_seq = seq;
    s_active = target;
    return CERT_STORE_OK;
}

/* ------------------------------------------------------------------------- */
/* Public API                                                                  */
/* ------------------------------------------------------------------------- */

/* Load the newest valid slot into the RAM copy and decrypt it; an empty store leaves it zeroed. */
static void load_newest(void)
{
    slot_header_t hdr;
    uint32_t best = SLOT_COUNT; /* none yet */
    uint32_t best_seq = 0U;

    (void) memset(&s_body, 0, sizeof(s_body));
    s_seq = 0U;
    s_active = SLOT_COUNT - 1U; /* the first write goes to slot 0 */
    for (uint32_t slot = 0U; slot < SLOT_COUNT; slot++)
    {
        if (slot_valid(slot, &hdr) && ((best == SLOT_COUNT) || (hdr.sequence > best_seq)))
        {
            best = slot;
            best_seq = hdr.sequence;
        }
    }
    if ((best < SLOT_COUNT) && (s_flash->read(slot_addr(best) + sizeof(hdr), (uint8_t *) &s_body,
                                              (uint32_t) sizeof(s_body)) == QSPI_FLASH_OK))
    {
        s_seq = best_seq;
        s_active = best;
        crypt_secrets(s_seq);
    }
    else
    {
        (void) memset(&s_body, 0, sizeof(s_body));
    }
}

cert_store_err_t cert_store_init(const iqspi_flash_t *flash,
                                 const uint8_t uid[CERT_STORE_UID_BYTES])
{
    if ((flash == NULL) || (uid == NULL))
    {
        return CERT_STORE_ERR_ARG;
    }
    s_flash = flash;
    (void) memcpy(s_uid, uid, CERT_STORE_UID_BYTES);
    load_newest();
    s_inited = true;
    return CERT_STORE_OK;
}

bool cert_store_has_wifi(void)
{
    return s_inited && (s_body.ssid_len > 0U);
}

const char *cert_store_ssid(void)
{
    return s_body.ssid;
}

const char *cert_store_password(void)
{
    return s_body.pass;
}

const uint8_t *cert_store_item(cert_store_item_t item, uint32_t *len)
{
    const uint8_t *p = NULL;
    uint32_t n = 0U;

    if (s_inited)
    {
        switch (item)
        {
        case CERT_STORE_CA:
            p = s_body.ca;
            n = s_body.ca_len;
            break;
        case CERT_STORE_CLIENT:
            p = s_body.client;
            n = s_body.client_len;
            break;
        case CERT_STORE_KEY:
            p = s_body.key;
            n = s_body.key_len;
            break;
        default:
            break;
        }
    }
    if (len != NULL)
    {
        *len = n;
    }
    return (n > 0U) ? p : NULL;
}

const char *cert_store_endpoint(void)
{
    return s_body.endpoint;
}

cert_store_err_t cert_store_set_endpoint(const char *host)
{
    if (!s_inited)
    {
        return CERT_STORE_ERR_NOT_INIT;
    }
    if (host == NULL)
    {
        return CERT_STORE_ERR_ARG;
    }
    const size_t n = strlen(host);

    if ((n == 0U) || (n > CERT_STORE_ENDPOINT_MAX))
    {
        return CERT_STORE_ERR_ARG;
    }
    for (size_t i = 0U; i < n; i++)
    {
        const char c = host[i];
        const bool ok = ((c >= 'a') && (c <= 'z')) || ((c >= 'A') && (c <= 'Z')) ||
                        ((c >= '0') && (c <= '9')) || (c == '.') || (c == '-');

        if (!ok)
        {
            return CERT_STORE_ERR_ARG;
        }
    }
    (void) memset(s_body.endpoint, 0, sizeof(s_body.endpoint));
    (void) memcpy(s_body.endpoint, host, n);
    s_body.endpoint_len = (uint8_t) n;

    const cert_store_err_t err = save();

    if (err != CERT_STORE_OK)
    {
        load_newest(); /* back to the copy that is still on flash */
    }
    return err;
}

bool cert_store_identity_complete(void)
{
    return s_inited && (s_body.ca_len > 0U) && (s_body.client_len > 0U) && (s_body.key_len > 0U);
}

static bool printable(const char *s, uint32_t min, uint32_t max)
{
    const size_t n = strlen(s);

    if ((n < min) || (n > max))
    {
        return false;
    }
    for (size_t i = 0U; i < n; i++)
    {
        if (((unsigned char) s[i] < 0x20U) || ((unsigned char) s[i] > 0x7EU))
        {
            return false;
        }
    }
    return true;
}

cert_store_err_t cert_store_set_wifi(const char *ssid, const char *pass)
{
    if (!s_inited)
    {
        return CERT_STORE_ERR_NOT_INIT;
    }
    if ((ssid == NULL) || (pass == NULL) || !printable(ssid, 1U, CERT_STORE_SSID_MAX) ||
        !printable(pass, 8U, CERT_STORE_PASS_MAX))
    {
        return CERT_STORE_ERR_ARG;
    }
    (void) memset(s_body.ssid, 0, sizeof(s_body.ssid));
    (void) memset(s_body.pass, 0, sizeof(s_body.pass));
    (void) memcpy(s_body.ssid, ssid, strlen(ssid));
    (void) memcpy(s_body.pass, pass, strlen(pass));
    s_body.ssid_len = (uint8_t) strlen(ssid);
    s_body.pass_len = (uint8_t) strlen(pass);

    const cert_store_err_t err = save();

    if (err != CERT_STORE_OK)
    {
        load_newest(); /* back to the copy that is still on flash */
    }
    return err;
}

/* A DER structure starts with a SEQUENCE tag, and its encoded length is the rest of the buffer. */
static bool der_ok(const uint8_t *der, uint32_t len)
{
    if ((len < 2U) || (der[0] != DER_SEQUENCE))
    {
        return false;
    }
    uint32_t body = der[1];
    uint32_t hdr = 2U;

    if ((der[1] & DER_LONG_FORM) != 0U)
    {
        const uint32_t n = der[1] & DER_LEN_BYTES_MASK;

        if ((n == 0U) || (n > 2U) || (len < (2U + n)))
        {
            return false;
        }
        body = 0U;
        for (uint32_t i = 0U; i < n; i++)
        {
            body = (body << 8U) | der[2U + i];
        }
        hdr = 2U + n;
    }
    return (hdr + body) == len;
}

cert_store_err_t cert_store_set_item(cert_store_item_t item, const uint8_t *der, uint32_t len)
{
    if (!s_inited)
    {
        return CERT_STORE_ERR_NOT_INIT;
    }
    if ((der == NULL) || (len == 0U) || ((unsigned) item > (unsigned) CERT_STORE_KEY))
    {
        return CERT_STORE_ERR_ARG;
    }
    const uint32_t cap = (item == CERT_STORE_KEY) ? CERT_STORE_KEY_MAX : CERT_STORE_CERT_MAX;

    if (len > cap)
    {
        return CERT_STORE_ERR_TOO_BIG;
    }
    if (!der_ok(der, len))
    {
        return CERT_STORE_ERR_FORMAT;
    }
    switch (item)
    {
    case CERT_STORE_CA:
        (void) memset(s_body.ca, 0, sizeof(s_body.ca));
        (void) memcpy(s_body.ca, der, len);
        s_body.ca_len = (uint16_t) len;
        break;
    case CERT_STORE_CLIENT:
        (void) memset(s_body.client, 0, sizeof(s_body.client));
        (void) memcpy(s_body.client, der, len);
        s_body.client_len = (uint16_t) len;
        break;
    default:
        (void) memset(s_body.key, 0, sizeof(s_body.key));
        (void) memcpy(s_body.key, der, len);
        s_body.key_len = (uint16_t) len;
        break;
    }
    const cert_store_err_t err = save();

    if (err != CERT_STORE_OK)
    {
        load_newest(); /* back to the copy that is still on flash */
    }
    return err;
}

cert_store_err_t cert_store_clear(void)
{
    if (!s_inited)
    {
        return CERT_STORE_ERR_NOT_INIT;
    }
    (void) memset(&s_body, 0, sizeof(s_body));
    const cert_store_err_t err = save();

    if (err != CERT_STORE_OK)
    {
        load_newest(); /* back to the copy that is still on flash */
    }
    return err;
}

uint32_t cert_store_sequence(void)
{
    return s_seq;
}
