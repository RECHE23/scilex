# Builds the abi3 C++ extension for the SciLex Python binding. Run from the
# repository root (the sdist/wheel build entry); metadata lives in pyproject.toml.
# SciLex is header-only (headers in include/) and depends on REAL's headers,
# located through the installed real-regex package, or a sibling checkout.
import os
import shutil
import sys

from setuptools import Extension, setup
from setuptools.command.build_py import build_py

try:  # setuptools >= 70.1 vendors bdist_wheel; older installs get it from wheel
    from setuptools.command.bdist_wheel import bdist_wheel
except ImportError:  # pragma: no cover
    from wheel.bdist_wheel import bdist_wheel

HERE = os.path.dirname(os.path.abspath(__file__))


def real_include():
    """REAL's header directory: from the installed package, else a sibling checkout."""
    try:
        import real
        return real.get_include()
    except Exception:  # pragma: no cover - fallback for a source checkout
        return os.path.normpath(os.path.join(HERE, os.pardir, "real-v1", "include"))


class build_py_with_headers(build_py):
    """Ships the header-only C++ library inside the package so an installed
    wheel can be located by scilex.get_include() for C++ integration."""

    def run(self):
        super().run()
        src = os.path.join(HERE, "include", "scilex")
        dst = os.path.join(self.build_lib, "scilex", "include", "scilex")
        os.makedirs(dst, exist_ok=True)
        for name in sorted(os.listdir(src)):
            if name.endswith(".hpp"):
                shutil.copy2(os.path.join(src, name), os.path.join(dst, name))


class abi3_wheel(bdist_wheel):
    """Forces the stable-ABI tag so one cp310-abi3 wheel serves CPython 3.10+."""

    def finalize_options(self):
        super().finalize_options()
        self.py_limited_api = "cp310"


if sys.platform == "win32":
    compile_args = ["/std:c++20", "/O2", "/EHsc", "/Zc:__cplusplus"]
else:
    compile_args = ["-std=c++20", "-O2", "-fvisibility=hidden"]

setup(
    cmdclass={"bdist_wheel": abi3_wheel, "build_py": build_py_with_headers},
    ext_modules=[
        Extension(
            "scilex._scilex",
            sources=["python/src/_scilex.cpp"],
            include_dirs=["include", real_include()],
            extra_compile_args=compile_args,
            define_macros=[("Py_LIMITED_API", "0x030A0000")],
            py_limited_api=True,
        )
    ],
)
