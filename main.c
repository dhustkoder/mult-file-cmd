#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <stdbool.h>
#include <unistd.h>
#include <dirent.h>
#include <pthread.h>


#define log(...) do { fprintf(stdout, __VA_ARGS__); fprintf(stdout, "\n"); } while (0)
#define log_error(...) do { fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } while (0)


struct command {
	const char* prog;
	char** args;
	int nargs;
	int phidx;
};

struct directory {
	DIR* dirp;
	const char* path;
	int nfiles;
};


static struct directory directory;
static struct command command;
static pthread_mutex_t lock;

static bool mfcmd_init(int argc, char** argv)
{
	/* alloc aux buffer on stack */
	command.prog = argv[2];
	char buffer[strlen("which ") + strlen(command.prog) + 1];


	/* test if directory is valid */
	directory.path = argv[1];
	directory.dirp = opendir(directory.path);
	if (!directory.dirp) {
		log_error("invalid directory %s", directory.path);
		return false;
	}

	/* get how many files */
	struct dirent* entry = NULL;
	while ((entry = readdir(directory.dirp)) != NULL) {
		if (entry->d_type == DT_REG)
			++directory.nfiles;
	}

	rewinddir(directory.dirp);

	if (directory.nfiles == 0) {
		log_error("no files in directory %s", directory.path);
		goto Lclosedir;
	}
	
	/* test if command's program is valid */
	strcpy(buffer, "which ");
	strcat(buffer, command.prog);

	if (system(buffer) != 0) {
		log_error("invalid command");
		goto Lclosedir;
	}

	command.args = argv + 3;
	command.nargs = argc - 3;
	command.phidx = -1;

	/* find the placeholder index in argument list */
	for (int i = 0; i < command.nargs; ++i) {
		if (strcmp(command.args[i], "$") == 0)
			command.phidx = i;
	}

	if (command.phidx == -1) {
		log_error("file placeholder not found in argument list.\n"
		          "use $ for specifying the file in the command argument list");
		goto Lclosedir;
	}

	/* initialize the mutex for multi threaded calls */
	if (pthread_mutex_init(&lock, NULL) != 0) {
		log_error("failed to init mutex");
		goto Lclosedir;
	}

	log(
		"INFO: \n"
		"DIRECTORY: %s\n"
		"NUMBER OF FILES TO PROCESS: %d",
		directory.path,
		directory.nfiles
	);


	return true;

Lclosedir:
	closedir(directory.dirp);

	return false;
}


static void mfcmd_term(void)
{
	closedir(directory.dirp);
}


static bool fetch_next_file(char** dest)
{
	pthread_mutex_lock(&lock);

	struct dirent* entry;

	while ((entry = readdir(directory.dirp)) != NULL) {
		/* check if it is a regular file (not a directory etc...) */
		if (entry->d_type == DT_REG)
			break;
	}

	if (entry != NULL) 
		strcpy(*dest, entry->d_name);

	pthread_mutex_unlock(&lock);

	return entry != NULL;
}


static void* command_executer(void* dum)
{
	char cmdbuffer[256] = { 0 };
	char pathbuffer[256] = { 0 };
	char result[512] = { 0 };
	char* p = pathbuffer;

	/* prepare the PRE filename arguments for the command */
	strcpy(cmdbuffer, command.prog);
	for (int i = 0; i < command.phidx; ++i) {
		strcat(cmdbuffer, " ");
		strcat(cmdbuffer, command.args[i]);
	}

	while (fetch_next_file(&p)) {
		strcpy(result, cmdbuffer);
		
		strcat(result, " ");
		strcat(result, directory.path);
		strcat(result, "/");
		strcat(result, p);

		/* add the next arguments after the filename */
		for (int i = command.phidx + 1; i < command.nargs; ++i) {
			strcat(result, " ");
			strcat(result, command.args[i]);
		}

		/* execute the command */
		system(result);
	}

	return NULL;
}


int main(int argc, char** argv)
{
	if (argc < 3) {
		log_error("Usage: %s [directory] [command] ...command args with $ for file placeholder", argv[0]);
		return EXIT_FAILURE;
	}

	if (!mfcmd_init(argc, argv)) {
		return EXIT_FAILURE;
	}

	pthread_t thr[4];

	pthread_create(&thr[0], NULL, &command_executer, NULL);
	pthread_create(&thr[1], NULL, &command_executer, NULL);
	pthread_create(&thr[2], NULL, &command_executer, NULL);
	pthread_create(&thr[3], NULL, &command_executer, NULL);


	pthread_join(thr[0], NULL);
	pthread_join(thr[1], NULL);
	pthread_join(thr[2], NULL);
	pthread_join(thr[3], NULL);

	mfcmd_term();
	return EXIT_SUCCESS;
}

