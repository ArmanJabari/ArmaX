TARGET   = Armax-Display.exe
SRC      = src/main.cpp \
           imgui/imgui.cpp imgui/imgui_draw.cpp imgui/imgui_tables.cpp imgui/imgui_widgets.cpp \
           imgui/imgui_impl_win32.cpp imgui/imgui_impl_dx9.cpp
OBJ      = $(SRC:.cpp=.o)
CXXFLAGS = -Iimgui -O2 -MMD -MP
LDFLAGS  = -mwindows -static
LDLIBS   = -ld3d9 -ldwmapi -lgdi32 -limm32

$(TARGET): $(OBJ)
	g++ $(OBJ) -o $@ $(LDFLAGS) $(LDLIBS)

%.o: %.cpp
	g++ $(CXXFLAGS) -c $< -o $@

clean:
	del /Q src\*.o src\*.d imgui\*.o imgui\*.d $(TARGET)

-include $(OBJ:.o=.d)