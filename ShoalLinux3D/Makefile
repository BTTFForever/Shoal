CXX ?= g++
CXXFLAGS ?= -O2 -std=c++17 -Wall
SHADERS := $(wildcard shaders/*)

shoal3d: shoal3d.cpp shaders_gen.h
	$(CXX) $(CXXFLAGS) $$(sdl2-config --cflags) shoal3d.cpp -o shoal3d $$(sdl2-config --libs) -lGLEW -lGL

shaders_gen.h: $(SHADERS)
	rm -f $@; for f in $(SHADERS); do xxd -i $$f >> $@; done
	printf '\n' >> $@

install: shoal3d
	install -Dm755 shoal3d $(HOME)/.local/bin/shoal3d; install -Dm755 shoal-settings.py $(HOME)/.local/bin/shoal-settings.py

clean:
	rm -f shoal3d shaders_gen.h
