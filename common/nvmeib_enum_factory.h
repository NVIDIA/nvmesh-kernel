#ifndef NVMEIB__ENUM_FACTORY_H
#define NVMEIB__ENUM_FACTORY_H
/*  This file implements a technique for making enum and a list of string with
	identical names to enum values.
	Example making: enum { red=0, blue=1, green=2} color; and to_string function
	will require a code like: switch (c) case red: return "red"; case blue:...
	Enum fuctory can generate all this code without typing values twice.
	Based on the code form:
	http://stackoverflow.com/questions/147267/easy-way-to-use-variables-of-enum-types-as-string-in-c
	http://stackoverflow.com/questions/9150538/how-do-i-tostring-an-enum-in-c
	Usage Example: In your .h file type:
		#define MY_COLORS(XX)  XX(RED,=0) XX(BLUE,) XX(GREEN,=5)
		DECLARE_ENUM(nvmeib_colors, MY_COLORS);
	In .c file type
    	DEFINE_ENUM( nvmeib_colors, MY_COLORS);
    	int test(){
			enum nvmeib_colors color = BLUE;
    		printk("%s %s %d %d\n",
    		nvmeib_colors_to_string(color), nvmeib_colors_to_string(-2),
    		nvmeib_colors_from_string("GREEN"), nvmeib_colors_from_string("A"));
    	}
    	// Output: BLUE ??? 5 -1
*/


#define ENUM_VALUE( name, assign) name assign,							// expansion macro for enum value definition
#define ENUM_CASE(  name, assign) case name: return #name;				// expansion macro for enum to string conversion
#define ENUM_STRCMP(name, assign) if (!strcmp(str,#name)) return name;	// expansion macro for string to enum conversion

/* Declare the access function and define enum values (use in .h file) */
#define DECLARE_ENUM(e_type, ENUM_DEF)	\
	enum e_type { ENUM_DEF(ENUM_VALUE) };	\
	const char * e_type##_to_string(  enum e_type dummy);	\
	enum e_type  e_type##_from_string(const char *str);

/* Define the access function names */
#define DEFINE_ENUM(e_type, ENUM_DEF) \
	const char * e_type##_to_string(enum e_type value){ \
		switch (value){ ENUM_DEF(ENUM_CASE) \
		default: return "???"; /* handle input error */ \
		} \
	} \
	enum e_type e_type##_from_string(const char *str){ \
		ENUM_DEF(ENUM_STRCMP) \
		return (enum e_type)-1; /* handle input error */ \
  }

#endif
