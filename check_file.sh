#!/bin/bash
FILE_TO_CHECK="../drivers/starkernel/Kconfig"
if [ ! -f "$FILE_TO_CHECK" ]; then
    echo "" > ../drivers/starkernel/Kconfig
    echo "" > ../drivers/starkernel/Makefile
fi
