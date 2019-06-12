#!/bin/bash

gcc -fplugin=../gcc-plugin/multiverse.so test.c -I../libmultiverse -L../libmultiverse -lmultiverse -o test

