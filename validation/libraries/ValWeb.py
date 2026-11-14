"""Robot library: the Remote Operator's browser (Playwright, headless Chromium) on the local cloud web app.

Settings (environment): VAL_HOST (default envmon.lan), VAL_IP (the Pi), VAL_USER, VAL_PASS; VAL_SHOTS=dir keeps screenshots.
"""
import os

from playwright.sync_api import sync_playwright

HOST = os.environ.get("VAL_HOST", "envmon.lan")
IP = os.environ.get("VAL_IP", "192.168.0.209")
SHOTS = os.environ.get("VAL_SHOTS", os.path.join(os.path.dirname(__file__), "..", "build", "shots"))


class ValWeb:
    ROBOT_LIBRARY_SCOPE = "SUITE"

    def __init__(self):
        self.pw = self.browser = self.ctx = self.page = None

    def open_browser(self, width=1280, height=900):
        self.pw = sync_playwright().start()
        self.browser = self.pw.chromium.launch(args=["--host-resolver-rules=MAP %s %s" % (HOST, IP)])
        self.ctx = self.browser.new_context(ignore_https_errors=True, viewport={"width": int(width), "height": int(height)})
        self.page = self.ctx.new_page()
        self.page.set_default_timeout(15000)
        os.makedirs(SHOTS, exist_ok=True)

    def close_browser(self):
        for o in (self.ctx, self.browser):
            if o:
                o.close()
        if self.pw:
            self.pw.stop()

    def new_visitor(self):
        """A fresh context without cookies (a person who is not signed in)."""
        if self.ctx:
            self.ctx.close()
        self.ctx = self.browser.new_context(ignore_https_errors=True)
        self.page = self.ctx.new_page()
        self.page.set_default_timeout(15000)

    def go(self, path="/"):
        self.page.goto("https://%s%s" % (HOST, path))

    def screenshot(self, name):
        self.page.screenshot(path=os.path.join(SHOTS, name + ".png"), full_page=True)

    def sign_in(self, user, password):
        self.page.get_by_label("User").fill(user)
        self.page.get_by_label("Password").fill(password)
        self.page.get_by_role("button", name="Sign in").click()

    def sign_in_as_validator(self):
        self.sign_in(os.environ.get("VAL_USER", "validator"), os.environ["VAL_PASS"])

    def text_visible(self, text, timeout=15000):
        self.page.get_by_text(text, exact=False).first.wait_for(timeout=float(timeout))

    def text_not_present(self, text):
        return self.page.get_by_text(text, exact=False).count() == 0

    def click_button(self, name, exact=True):
        self.page.get_by_role("button", name=name, exact=bool(exact)).first.click()

    def open_tab(self, name):
        self.click_button(name)

    def body_text(self):
        return self.page.inner_text("body")

    def count(self, css):
        return self.page.locator(css).count()

    def big_values(self):
        """The four big readings of the overview, as text."""
        return self.page.locator(".big").all_inner_texts()

    def chart_has_data(self, label):
        """True when the chart named `label` has at least one drawn data path."""
        svg = self.page.get_by_role("img", name="%s over time" % label)
        svg.wait_for()
        try:
            svg.locator("path[stroke-width='2']").first.wait_for(timeout=10000)
        except Exception:
            return False
        return True

    def table_rows(self, heading):
        card = self.page.locator("div.card", has=self.page.get_by_role("heading", name=heading))
        return [r.inner_text().replace("\t", " | ") for r in card.locator("tbody tr").all()]

    def wait_for_row_containing(self, heading, text, timeout=20000):
        card = self.page.locator("div.card", has=self.page.get_by_role("heading", name=heading))
        card.locator("tbody tr", has_text=text).first.wait_for(timeout=float(timeout))
