all: reimu-hwmon

# rbuf.c, dbusjson.c: ../reimu-common/files in the tree (bitbake puts them here)
COMMON ?= ../../reimu-common/files
VPATH = $(COMMON)
CFLAGS += -I$(COMMON)

SRC = reimu-hwmon.c hwmon-dt.c hwmon-chips.c hwmon-model.c sensorbus.c reimu-overheat.c reimu-view-live.c rbuf.c dbusjson.c
HDR = hwmon.h sensorbus.h reimu-overheat.h reimu-view-live.h

reimu-hwmon: $(SRC) $(HDR)
	$(CC) $(CFLAGS) -Wall -o $@ $(filter %.c,$^) -lsystemd -lm $(LDFLAGS)

# native, on the build host: the device trees, the chips (fake), the model
HOSTCC ?= gcc
IPMI_YAML ?= ../../../../meta-ab64/recipes-phosphor/ipmi/reimu-ab-ipmi-sensor-inventory/config.yaml
test: tests/test_hwmon
	IPMI_YAML=$(IPMI_YAML) ./tests/test_hwmon

tests/test_hwmon: tests/test_hwmon.c hwmon-dt.c hwmon-chips.c hwmon-model.c rbuf.c hwmon.h
	$(HOSTCC) -Wall -Wextra -g -O1 -fsanitize=address,undefined -I. -I$(COMMON) -o $@ $(filter %.c,$^) -lm

clean:
	rm -f reimu-hwmon tests/test_hwmon
