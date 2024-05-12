CC = g++

OUT = ../build/utils

all: bundler

bundler:
	@mkdir -p $(OUT)
	$(CC) bundler.cpp -o $(OUT)/bundler
