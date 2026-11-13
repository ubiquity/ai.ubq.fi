#!/usr/bin/python3
"""Replace this process with the one compiler under the approved VM ceiling."""
import os
import resource
import sys
resource.setrlimit(resource.RLIMIT_AS, (1073741824, 1073741824))
os.execv(sys.argv[1], sys.argv[1:])
