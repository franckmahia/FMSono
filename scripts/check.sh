#!/bin/bash
set -euo pipefail
PROJECT_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$PROJECT_ROOT"
if [ ! -x .venv/bin/pytest ]; then
    python3 -m venv .venv
    .venv/bin/pip install -r requirements-dev.txt
fi
.venv/bin/pytest
.venv/bin/ruff check tests
.venv/bin/ruff format --check tests
clang -std=c11 -Wall -Wextra -Werror -fsyntax-only -I Sources/SonoDSP/include Sources/SonoDSP/Processor.c Sources/SonoDSP/Hardware.c
bash -n scripts/build-app.sh scripts/check.sh 'Lancer FMSono.command'
if git rev-parse --show-toplevel >/dev/null 2>&1; then
    git diff --check
else
    # Check the source tree even before this new project has a Git repository.
    # --no-index returns 1 for a clean addition; whitespace errors set another bit.
    check_new_file() {
        git diff --no-index --check /dev/null "$1" || [ "$?" -eq 1 ]
    }
    while IFS= read -r -d '' source; do
        check_new_file "$source"
    done < <(find Sources scripts tests Packaging -type f \( -name '*.swift' -o -name '*.c' -o -name '*.h' -o -name '*.py' -o -name '*.sh' -o -name '*.plist' -o -name '*.modulemap' \) -print0)
    for source in Package.swift README.md .gitignore pytest.ini requirements-dev.txt 'Lancer FMSono.command'; do
        check_new_file "$source"
    done
fi
