import pytest

from iotcloud.config import Settings
from iotcloud.db import Database


@pytest.fixture()
def db(tmp_path):
    return Database(str(tmp_path / "t.sqlite3"))


@pytest.fixture()
def settings(tmp_path):
    return Settings(db_path=str(tmp_path / "t.sqlite3"), online_window_s=150, web_dir="")
