#include <stdbool.h>
#define NOB_IMPLEMENTATION
#define NOB_WARN_DEPRECATED
#include "nob.h"

#include "os.h"

#define BUILD_DIR "build"
#define SRC_DIR "."
#define VENDOR_DIR "vendor"
#define CIMGUI_DIR "cimgui"
#define CIMGUI_UNITY "cimgui_unity.cpp"
#define CIMGUI_OBJ "cimgui.o"
#define DELTA (1 << 3)
#define GLFW_INCLUDE "-I./vendor/glfw-3.4/include/"
#define GLFW_LINUX_LIB "./vendor/glfw-3.4/lib_linux/libglfw3.a"
#define GLFW_WIN64_LIB "./vendor/glfw-3.4/lib_win64/libglfw3.a"
#define LIBTOM_CRYPT "./vendor/libtomcrypt-1.18.2/libtomcrypt.a" 
#define LIBTOM_CRYPT_INCLUDE "-I./vendor/libtomcrypt-1.18.2/src/headers/" 
#define SQLCHIPER "./"BUILD_DIR"/sqlite3.o"
char cwd[OS_PATHMAX];
char build_dir[OS_PATHMAX];

char *CC = "gcc";
char *CXX = "g++";
bool cross_compile = false;
char cimgui_include[OS_PATHMAX + DELTA] = "-I";


bool compile_cimgui()
{

	Cmd cmd = {0};
	char cimgui_unity[OS_PATHMAX];
	os_path_join(cimgui_unity, sizeof(cimgui_unity), cwd, SRC_DIR,
		     CIMGUI_UNITY);
	if (!os_exists(cimgui_unity)) {
		nob_log(NOB_ERROR, "%s not found.", cimgui_unity);
		return false;
	}

	char imgui_include[OS_PATHMAX + DELTA] = "-I";
	os_path_join(imgui_include + strlen(imgui_include),
		     sizeof(imgui_include), cwd, SRC_DIR, VENDOR_DIR,
		     CIMGUI_DIR, "imgui");
	char out[OS_PATHMAX];
	os_path_join(out, sizeof(out), build_dir, CIMGUI_OBJ);

	nob_cmd_append(&cmd, CXX, GLFW_INCLUDE, cimgui_include, imgui_include,
		       "-c", cimgui_unity, "-o", out, "-O2");
	if (!nob_cmd_run(&cmd))
		return false;
}

int main(int argc, char *argv[])
{
	NOB_GO_REBUILD_URSELF(argc, argv);
	int run = false;
	if (argc > 1) {
		for (int i = 1; i < argc; i++) {
			if (strcmp(argv[i], "--cross-compile") == 0) {
				CC = "x86_64-w64-mingw32-gcc";
				CXX = "x86_64-w64-mingw32-g++";
				cross_compile = true;
			}else if(strcmp(argv[i],"-run")==0){
				run = true;
			}
		}
	}
	os_getcwd(cwd, sizeof(cwd));
	printf("Current working dir: %s\n", cwd);

	os_path_join(build_dir, sizeof(build_dir), cwd, BUILD_DIR);
	printf("Build Dir: %s\n", build_dir);

	os_path_join(cimgui_include + strlen(cimgui_include),
		     sizeof(cimgui_include), cwd, SRC_DIR, VENDOR_DIR,
		     CIMGUI_DIR);

	if (!mkdir_if_not_exists(BUILD_DIR))
		return 1;
	char libcimgui[OS_PATHMAX];
	os_path_join(libcimgui, sizeof(libcimgui), BUILD_DIR, CIMGUI_OBJ);
	if (!os_exists(libcimgui)) {
		printf("%s not found. Creating %s\n", CIMGUI_OBJ, CIMGUI_OBJ);
		compile_cimgui();
	}

	if(!os_exists(LIBTOM_CRYPT)){
		Cmd cmd = {0};

		char buff[1<<8];
		snprintf(buff,sizeof(buff),"-j%d",nob_nprocs());
		const char* curr_dir = nob_get_current_dir_temp();
		nob_set_current_dir("./vendor/libtomcrypt-1.18.2/");
		nob_cmd_append(&cmd,"make",buff); 
		if (!nob_cmd_run(&cmd))
			return -1;
		nob_set_current_dir(curr_dir);
	}

	if(!os_exists(SQLCHIPER)){
		Cmd cmd = {0};
		nob_cmd_append(&cmd,CC,"-c","./vendor/sqlite3.c",
				"-DSQLITE_TEMP_STORE=2",
				"-DSQLITE_HAS_CODEC",
				"-DSQLITE_ENABLE_FTS5",
				"-DSQLITE_EXTRA_INIT=sqlcipher_extra_init",
				"-DSQLITE_EXTRA_SHUTDOWN=sqlcipher_extra_shutdown",
				"-DSQLCIPHER_CRYPTO_LIBTOMCRYPT",
				LIBTOM_CRYPT_INCLUDE,"-o",SQLCHIPER,"-O2");
		if (!nob_cmd_run(&cmd))
			return -1;
	}
	
	char *binary_name = BUILD_DIR "/embos";
	if (cross_compile)
		binary_name = BUILD_DIR "/embos.exe";

	Cmd cmd = {0};

	nob_cmd_append(&cmd, CC, "-DCIMGUI_USE_GLFW", "-DCIMGUI_USE_OPENGL3",
		       cimgui_include, GLFW_INCLUDE, "-I./vendor/", "-o", BUILD_DIR "/main.o",
				"-DSQLITE_TEMP_STORE=2",
				"-DSQLITE_HAS_CODEC",
				"-DSQLITE_EXTRA_INIT=sqlcipher_extra_init",
				"-DSQLITE_ENABLE_FTS5",
				"-DSQLITE_EXTRA_SHUTDOWN=sqlcipher_extra_shutdown",
				"-DSQLCIPHER_CRYPTO_LIBTOMCRYPT",
		       // "-c", "./main.c", "-Wall","-O2","-s");
		        "-c", "./main.c", "-Wall","-ggdb"); 

	if (!nob_cmd_run(&cmd))
		return false;
	if (cross_compile) {

		nob_cmd_append(&cmd, CXX, "-o", binary_name,
			       BUILD_DIR "/cimgui.o", BUILD_DIR "/main.o",
			       GLFW_WIN64_LIB, "-lopengl32", "-lgdi32","-mwindows",
			       "-static", "-static-libstdc++",
			       "-static-libgcc");
	} else {

		nob_cmd_append(&cmd, CXX, "-o", binary_name,
			       BUILD_DIR "/cimgui.o", BUILD_DIR "/main.o",SQLCHIPER,
			       GLFW_LINUX_LIB, LIBTOM_CRYPT,"-lm", "-lpthread",
			       "-static-libstdc++", "-static-libgcc","-flto");
	}

	if (!nob_cmd_run(&cmd))
		return false;
	if(run){
		nob_cmd_append(&cmd,"./build/embos");
		nob_cmd_run(&cmd);
	}
	return 0;
}
