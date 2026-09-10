"""Stamp the firmware with the commit it was built from.

PlatformIO runs this before every build (see extra_scripts in
platformio.ini).  It defines GIT_REV and GIT_DIRTY, which config.h turns into
what the control page and `version` report.

Everything here degrades quietly: a tarball with no .git, a machine with no
git installed, and a shallow clone all just leave the revision unknown rather
than failing a build that would otherwise have worked.
"""

import subprocess

Import("env")  # noqa: F821  -- injected by SCons


def git(*args):
    try:
        out = subprocess.run(
            ["git"] + list(args),
            cwd=env.subst("$PROJECT_DIR"),  # noqa: F821
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            timeout=10,
        )
    except (OSError, subprocess.SubprocessError):
        return None
    if out.returncode != 0:
        return None
    return out.stdout.decode("utf-8", "replace").strip()


rev = git("rev-parse", "--short", "HEAD") or "unknown"

# --porcelain prints one line per changed file, so any output at all means the
# build does not correspond to the commit named above.
status = git("status", "--porcelain")
dirty = 1 if status else 0

env.Append(CPPDEFINES=[  # noqa: F821
    ("GIT_REV", env.StringifyMacro(rev)),  # noqa: F821
    ("GIT_DIRTY", dirty),
])

print("git: building from %s%s" % (rev, "+dirty" if dirty else ""))
