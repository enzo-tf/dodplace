"""Entry point for ``python -m dodplace``.

The KiCad plugin shim calls the module this way rather than the console script,
because a process launched by KiCad does not inherit a shell PATH.
"""

import sys

from .cli import main

if __name__ == "__main__":
    sys.exit(main())
