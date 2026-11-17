CXX ?= g++
CXXFLAGS ?= -O2 -std=c++17 -Wall
CORE := ../ShoalCore
SHADERS := $(wildcard $(CORE)/shaders/*)
SOURCES := shoal.cpp $(CORE)/shoal_engine.cpp $(CORE)/shoal_linux.cpp shaders_gen.cpp

shoal: $(SOURCES) $(CORE)/shoal_engine.h $(CORE)/shoal_linux.h $(CORE)/shoal_gl.h
	$(CXX) $(CXXFLAGS) -I$(CORE) $$(pkg-config --cflags sdl2 SDL2_image) $(SOURCES) -o shoal \
		$$(pkg-config --libs sdl2 SDL2_image) -lOpenGL -lEGL

# The shaders live in ShoalCore/shaders (shared with the wallpaper, Windows and Android builds).
shaders_gen.cpp: $(SHADERS) $(CORE)/embed_shaders.py
	python3 $(CORE)/embed_shaders.py $(CORE)/shaders $@

install: shoal
	install -Dm755 shoal $(HOME)/.local/bin/shoal; install -Dm755 shoal-settings.py $(HOME)/.local/bin/shoal-settings.py

clean:
	rm -f shoal shaders_gen.cpp shaders_gen.h
