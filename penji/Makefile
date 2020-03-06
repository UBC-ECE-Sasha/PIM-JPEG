RM 						= rm -rf

CC 						= gcc
CFLAGS 					= -g -w -Wl,-rpath=./libjpeg-turbo/build/ 

COMMON_SOURCES			=
COMMON_INCLUDES 		= -I/libjpeg-turbo/ -Llibjpeg-turbo/build/ -ljpeg

REMOVE_LINES_SOURCES 	= remove_lines.c
REMOVE_LINES_INCLUDES	= $(COMMON_INCLUDES)
REMOVE_LINES_PROGRAM	= remove_lines

default: all

all: $(REMOVE_LINES_PROGRAM)

$(REMOVE_LINES_PROGRAM): $(REMOVE_LINES_SOURCES)
	$(CC) $(REMOVE_LINES_SOURCES) -o $(REMOVE_LINES_PROGRAM) $(CFLAGS) $(REMOVE_LINES_INCLUDES)


clean:
	$(RM) $(REMOVE_LINES_PROGRAM)
