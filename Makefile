CXX      := g++
TARGET   := Armax-Display.exe
CXXFLAGS := -std=c++17 -O3 -Iimgui -Isrc
LDFLAGS  := -mwindows -static
LDLIBS   := -ld3d9 -ldwmapi -lgdi32 -limm32

SRCS := src/main.cpp \
        imgui/imgui.cpp \
        imgui/imgui_draw.cpp \
        imgui/imgui_tables.cpp \
        imgui/imgui_widgets.cpp \
        imgui/imgui_impl_win32.cpp \
        imgui/imgui_impl_dx9.cpp

OBJS := $(SRCS:.cpp=.o)

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CXX) $(OBJS) -o $@ $(LDFLAGS) $(LDLIBS)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	-del /Q /F $(subst /,\,$(OBJS)) $(TARGET) 2>nul

.PHONY: all clean
