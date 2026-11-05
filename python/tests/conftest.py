import os
import pathlib
import sys

import pytest

HERE = pathlib.Path(__file__).resolve().parent
REPO_ROOT = pathlib.Path(os.environ.get("LOB_REPO_ROOT", HERE.parents[1])).resolve()

# Test helpers: the reference book (this directory) and the synthetic stream (python/bench).
for p in (HERE, REPO_ROOT / "python" / "bench"):
    if str(p) not in sys.path:
        sys.path.insert(0, str(p))


@pytest.fixture
def repo_root() -> pathlib.Path:
    return REPO_ROOT
