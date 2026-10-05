CXX ?= g++
CXXFLAGS ?= -O2
CXXFLAGS += -shared -fPIC -std=c++23 \
	$(shell pkg-config --cflags hyprgraphics pixman-1 libdrm hyprland pangocairo libinput libudev wayland-server xkbcommon)

all: hyprmosaic.so

hyprmosaic.so: hyprmosaic.cpp
	$(CXX) $(CXXFLAGS) $< -o $@

clean:
	rm -f hyprmosaic.so

.PHONY: all clean
