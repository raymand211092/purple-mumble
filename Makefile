CC       = gcc
CFLAGS  := $(shell pkg-config --cflags purple-3 libprotobuf-c) -fPIC -Wno-discarded-qualifiers
LDFLAGS := $(shell pkg-config --libs purple-3 libprotobuf-c)

OBJECTS = mumble-message.o mumble.pb-c.o plugin.o
PLUGIN  = mumble.so

.PHONY: clean install

$(PLUGIN): $(OBJECTS)
	gcc -shared $(LDFLAGS) -o $@ $^

clean:
	rm -f *.o $(PLUGIN)
	rm -f *~
