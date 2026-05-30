#!/bin/bash

TMAKE=qmake
./mkpro
$TMAKE -o Makefile smaragd.pro
make
