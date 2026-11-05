"""The type stubs (lobsim/__init__.pyi, lobsim/paper.pyi) agree with the runtime module.

1. Parameter names, order and keyword-only markers of every stubbed function and method equal the
   signature pybind11 writes on the first line of the runtime docstring. (pybind11 functions have
   no __text_signature__, so mypy.stubtest cannot check this.)
2. mypy.stubtest checks that names, classes and properties exist on both sides (skipped when
   mypy is not installed).
"""

import ast
import os
import pathlib
import subprocess
import sys

import pytest

import lobsim

HERE = pathlib.Path(__file__).resolve().parent
PKG = pathlib.Path(lobsim.__file__).resolve().parent


def stub_functions():
    out = []
    for stub, modname in ((PKG / "__init__.pyi", "lobsim"), (PKG / "paper.pyi", "lobsim.paper")):
        tree = ast.parse(stub.read_text())
        for node in tree.body:
            if isinstance(node, ast.FunctionDef):
                out.append((modname, None, node))
            elif isinstance(node, ast.ClassDef):
                for item in node.body:
                    if isinstance(item, ast.FunctionDef) and not any(
                            isinstance(d, ast.Name) and d.id == "property"
                            for d in item.decorator_list):
                        out.append((modname, node.name, item))
    return out


def stub_params(fn: ast.FunctionDef):
    a = fn.args
    names = [x.arg for x in a.posonlyargs + a.args]
    if a.kwonlyargs:
        names.append("*")
        names += [x.arg for x in a.kwonlyargs]
    return names


def split_top_level(s):
    parts, depth, cur = [], 0, ""
    for ch in s:
        if ch in "([{":
            depth += 1
        elif ch in ")]}":
            depth -= 1
        if ch == "," and depth == 0:
            parts.append(cur.strip())
            cur = ""
        else:
            cur += ch
    if cur.strip():
        parts.append(cur.strip())
    return parts


def runtime_params(obj):
    first = (obj.__doc__ or "").lstrip().splitlines()[0]
    start = first.index("(")
    depth = 0
    for i in range(start, len(first)):
        depth += first[i] == "("
        depth -= first[i] == ")"
        if depth == 0:
            break
    inner = first[start + 1:i]
    return [p.split(":")[0].split("=")[0].strip() for p in split_top_level(inner)]


CHECKED = [f for f in stub_functions()
           if f[2].name not in ("__eq__", "__hash__", "__len__")]  # protocol slots: no names


@pytest.mark.parametrize("modname,cls,fn", CHECKED,
                         ids=[f"{m}.{c + '.' if c else ''}{f.name}" for m, c, f in CHECKED])
def test_stub_parameter_names_match_runtime(modname, cls, fn):
    mod = sys.modules[modname]
    obj = getattr(getattr(mod, cls), fn.name) if cls else getattr(mod, fn.name)
    assert stub_params(fn) == runtime_params(obj), obj.__doc__.splitlines()[0]


def test_every_public_callable_is_stubbed():
    stubbed = {(m, c, f.name) for m, c, f in stub_functions()}
    for modname, names in (("lobsim", lobsim.__all__), ("lobsim.paper", lobsim.paper.__all__)):
        mod = sys.modules[modname]
        for name in names:
            obj = getattr(mod, name)
            if callable(obj) and not isinstance(obj, type):
                assert (modname, None, name) in stubbed
    for name, member in vars(lobsim.OrderBook).items():
        if not name.startswith("_") and callable(member):
            assert ("lobsim", "OrderBook", name) in stubbed, name


def test_stubtest(tmp_path):
    pytest.importorskip("mypy")
    # The subprocess runs in tmp_path, so a relative PYTHONPATH (as in python/README.md) would no
    # longer find the package: point it at the directory the tested lobsim was imported from.
    env = dict(os.environ)
    found_in = str(pathlib.Path(lobsim.__file__).resolve().parents[1])
    env["PYTHONPATH"] = os.pathsep.join(p for p in (found_in, env.get("PYTHONPATH")) if p)
    proc = subprocess.run(
        [sys.executable, "-m", "mypy.stubtest", "lobsim",
         "--allowlist", str(HERE / "stubtest_allowlist.txt")],
        cwd=tmp_path, env=env, capture_output=True, text=True)
    assert proc.returncode == 0, proc.stdout + proc.stderr
