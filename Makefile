CXXFLAGS=-std=c++0x -g -Wall
LDLIBS=-lpthread -lX11 -lXdamage -lXtst -lXinerama -lXcursor -lXfixes -lXext -lXrandr

ifndef NO_NVIDIA
  CXXFLAGS+= -DENABLE_NVCTRL
  LDLIBS+= -lXNVCtrl -lXext
endif

screenclone:
