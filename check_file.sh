#!/bin/bash
FILE_TO_CHECK="$1/drivers/starkernel/Kconfig"
if [ ! -f "$FILE_TO_CHECK" ]; then
    echo "" > $1/drivers/starkernel/Kconfig
    echo "" > $1/drivers/starkernel/Makefile
fi
