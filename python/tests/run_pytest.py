"""CTest entry point: run pytest on python/tests. Arguments are passed to pytest.

Exits with 77, which CTest reports as "skipped", if pytest is not installed or the extension has
not been built. Any other import error (for example a missing symbol) is a failure."""

import sys

SKIP = 77

try:
    import pytest
except ImportError:
    print("python_pytest: skipped, pytest is not installed")
    sys.exit(SKIP)

try:
    import lobsim  # noqa: F401
except ModuleNotFoundError as e:
    if e.name in ("lobsim", "lobsim._lobsim"):
        print(f"python_pytest: skipped, {e.name} not found ({e}); build the lobsim target")
        sys.exit(SKIP)
    raise

sys.exit(pytest.main(sys.argv[1:]))
