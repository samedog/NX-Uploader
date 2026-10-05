# Host-side unit tests. These build with the normal system compiler, not the
# devkitARM/devkitA64 one, so they run on the machine doing the build.
#
#   make -C tests        # build and run
#   make test            # same, from the repo root

HOSTCC  ?= cc
CFLAGS  ?= -Wall -Wextra -O2
INCLUDES = -I../includes
SRC      = ../src
OUT      = build

.PHONY: all run clean

all: run

run: $(OUT)/mptest $(OUT)/mptest_eexist $(OUT)/fmtest $(OUT)/cfgtest
	@echo "== multipart (normal) =="
	@cd $(OUT) && ./mptest
	@echo "== multipart (exFAT-like rename) =="
	@cd $(OUT) && ./mptest_eexist
	@echo "== fileman =="
	@cd $(OUT) && ./fmtest
	@echo "== config =="
	@cd $(OUT) && ./cfgtest

$(OUT):
	@mkdir -p $(OUT)

$(OUT)/mptest: mptest.c $(SRC)/multipart.c | $(OUT)
	$(HOSTCC) $(CFLAGS) $(INCLUDES) -o $@ mptest.c $(SRC)/multipart.c

# Same test, but multipart.c is built so rename() refuses to overwrite an
# existing name, mimicking libnx's exFAT device. Exercises the retry path.
$(OUT)/mptest_eexist: mptest.c $(SRC)/multipart.c | $(OUT)
	$(HOSTCC) $(CFLAGS) $(INCLUDES) -Drename=test_rename_multipart -c $(SRC)/multipart.c -o $(OUT)/mp_eexist.o
	$(HOSTCC) $(CFLAGS) $(INCLUDES) -c mptest.c -o $(OUT)/mptest_eexist.o
	$(HOSTCC) $(OUT)/mp_eexist.o $(OUT)/mptest_eexist.o -o $@

$(OUT)/fmtest: fmtest.c $(SRC)/fileman.c $(SRC)/config.c | $(OUT)
	$(HOSTCC) $(CFLAGS) $(INCLUDES) -o $@ fmtest.c $(SRC)/fileman.c $(SRC)/config.c

$(OUT)/cfgtest: cfgtest.c $(SRC)/config.c | $(OUT)
	$(HOSTCC) $(CFLAGS) $(INCLUDES) -o $@ cfgtest.c $(SRC)/config.c

clean:
	rm -rf $(OUT)
