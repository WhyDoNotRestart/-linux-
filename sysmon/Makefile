CC      ?= gcc
CFLAGS  ?= -O2 -std=c11 -D_GNU_SOURCE -Wall -Wextra
LDLIBS  ?= -pthread -lm
BUILD   := build
SRC     := src

all: $(BUILD)/sysmon $(BUILD)/sysmon-tui

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/assets.h: web/index.html | $(BUILD)
	xxd -i -n index_html web/index.html > $@

$(BUILD)/sysmon: $(SRC)/main.c $(SRC)/collect.c $(SRC)/http.c $(SRC)/json.c $(SRC)/audit.c $(SRC)/apps.c $(SRC)/state.h $(SRC)/http.h $(SRC)/audit.h $(SRC)/apps.h $(BUILD)/assets.h | $(BUILD)
	$(CC) $(CFLAGS) -I$(BUILD) -o $@ $(SRC)/main.c $(SRC)/collect.c $(SRC)/http.c $(SRC)/json.c $(SRC)/audit.c $(SRC)/apps.c $(LDLIBS)

$(BUILD)/sysmon-tui: $(SRC)/tui.c $(SRC)/collect.c $(SRC)/audit.c $(SRC)/apps.c $(SRC)/state.h $(SRC)/audit.h $(SRC)/apps.h | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $(SRC)/tui.c $(SRC)/collect.c $(SRC)/audit.c $(SRC)/apps.c -lncursesw -lm

run: $(BUILD)/sysmon
	./$(BUILD)/sysmon

clean:
	rm -rf $(BUILD)

.PHONY: all run clean
