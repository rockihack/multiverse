#!/bin/bash

objdump -b binary -m i386:x86-64 -D original.dump > original.disass
objdump -b binary -m i386:x86-64 -D patched.dump > patched.disass

