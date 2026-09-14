# Shared bootstrap for macOS/Linux wrappers: locate a Python 3.9+ interpreter
find_agent_python() {
    for name in python3 python; do
        if command -v "$name" >/dev/null 2>&1; then
            if "$name" -c 'import sys; sys.exit(0 if sys.version_info >= (3, 9) else 1)' 2>/dev/null; then
                command -v "$name"; return 0
            fi
        fi
    done
    for root in "/Users/Shared/Epic Games"/UE_* "$HOME"/UnrealEngine* /opt/UnrealEngine*; do
        for sub in Mac/bin/python3 Linux/bin/python3; do
            candidate="$root/Engine/Binaries/ThirdParty/Python3/$sub"
            if [ -x "$candidate" ]; then echo "$candidate"; return 0; fi
        done
    done
    return 1
}
