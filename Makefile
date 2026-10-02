# SPDX-License-Identifier: GPL-2.0-or-later
KVER ?= $(shell uname -r)
KDIR ?= /lib/modules/$(KVER)/build
CC ?= cc

.PHONY: all modules clean test check install uninstall
all: modules
modules:
	$(MAKE) -C $(KDIR) M=$(CURDIR) W=1 modules
clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean
	rm -rf build
test:
	mkdir -p build
	$(CC) -std=c11 -Wall -Wextra -Werror -pthread -g -fsanitize=address,undefined -Isrc tests/test_core.c src/ec_core.c -o build/test_core
	./build/test_core
	python3 tests/test_install.py
check:
	bash -n scripts/install.sh scripts/uninstall.sh
	shellcheck scripts/install.sh scripts/uninstall.sh
	$(KDIR)/scripts/checkpatch.pl --no-tree --strict -f src/asus_fan_ec.c src/ec_core.c src/ec_core.h
install:
	./scripts/install.sh
uninstall:
	./scripts/uninstall.sh
