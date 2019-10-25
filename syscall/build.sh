#!/bin/bash

gcc -O0 -fplugin=../gcc-plugin/multiverse.so test.c -I../libmultiverse -L../libmultiverse -lmultiverse -o test

