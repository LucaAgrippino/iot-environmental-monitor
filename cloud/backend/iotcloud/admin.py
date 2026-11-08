"""Manage users on the machine that runs the cloud:

    python -m iotcloud.admin add-user luca --role admin      (asks for the password)
    python -m iotcloud.admin add-user guest --role viewer
    python -m iotcloud.admin set-password luca
    python -m iotcloud.admin list
    python -m iotcloud.admin delete-user guest

The database is the one in IOTCLOUD_DB (or --db).
"""
import argparse
import getpass
import sys

from . import auth, config
from .db import Database


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--db", default=config.load().db_path)
    sub = ap.add_subparsers(dest="cmd", required=True)
    a = sub.add_parser("add-user")
    a.add_argument("name")
    a.add_argument("--role", choices=auth.ROLES, default="viewer")
    a.add_argument("--password-env", metavar="VAR", help="read the password from this environment variable (scripts)")
    s = sub.add_parser("set-password")
    s.add_argument("name")
    s.add_argument("--password-env", metavar="VAR")
    sub.add_parser("list")
    d = sub.add_parser("delete-user")
    d.add_argument("name")
    args = ap.parse_args(argv)
    db = Database(args.db)

    def password():
        if args.password_env:
            import os
            return os.environ[args.password_env]
        p = getpass.getpass("Password: ")
        if p != getpass.getpass("Again: "):
            raise auth.AuthError("the two passwords differ")
        return p

    try:
        if args.cmd == "add-user":
            auth.add_user(db, args.name, password(), args.role)
            print(f"user {args.name} ({args.role}) saved")
        elif args.cmd == "set-password":
            rows = db.query("SELECT role FROM users WHERE name=?", (args.name,))
            if not rows:
                raise auth.AuthError("no such user")
            auth.add_user(db, args.name, password(), rows[0]["role"])
            print("password changed; the user's sessions were closed")
        elif args.cmd == "list":
            for u in auth.list_users(db):
                print(f"{u['name']:20} {u['role']}")
        elif args.cmd == "delete-user":
            print("deleted" if auth.delete_user(db, args.name) else "no such user")
    except auth.AuthError as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
