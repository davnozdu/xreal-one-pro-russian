"""Reject Apple Tcl/Tk 8.5 before creating or reusing a build environment."""
import sys


def validate_versions(tcl_version, tk_version):
    for name, version in (("Tcl", tcl_version), ("Tk", tk_version)):
        if tuple(int(part) for part in str(version).split(".")[:2]) < (8, 6):
            raise RuntimeError(f"{name} {version} is unsupported; use Python with Tcl/Tk >= 8.6")


def main():
    import tkinter
    validate_versions(tkinter.TclVersion, tkinter.TkVersion)
    print(f"Build Python: {sys.executable}; Tcl {tkinter.TclVersion}; Tk {tkinter.TkVersion}")


if __name__ == "__main__":
    main()
