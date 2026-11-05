#include "tie_app/application.h"

#include "aeron/main.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int TieMain_Usage(const char* message, const char* argument) {
	if (message && argument)
		fprintf(stderr, "[tie-aeron] "), fprintf(stderr, message, argument), fputc('\n', stderr);
	else if (message)
		fprintf(stderr, "[tie-aeron] %s\n", message);
	fprintf(stderr, "Usage: tie [--tie95-data <dir>] [--tie98-data <dir>] [--resource-root <dir>] [--skip-to-mission <pilot>] [--headless] [--capture <file.png> [seconds]]\n");
	return 2;
}

static int TieMain_ParseOptions(int argc, char* argv[], TieLaunchOptions* out) {
	static const char* const names[] = { "--tie95-data", "--tie98-data", "--resource-root" };
	const char** destinations[] = { &out->tie95_data, &out->tie98_data, &out->resource_root };
	memset(out, 0, sizeof *out);
	for (int index = 1; index < argc; ++index) {
		const char* argument = argv[index];
		if (strcmp(argument, "--headless") == 0) {
			out->headless = true;
			continue;
		}
		if (strcmp(argument, "--capture") == 0) {
			/* --capture <file.png> [seconds]: save the flight view after that many seconds, then quit. */
			if (index + 1 >= argc || argv[index + 1][0] == '-')
				return TieMain_Usage("option requires a file name: %s", "--capture");
			out->capture_path = argv[++index];
			out->capture_after_seconds = 6.0f;
			if (index + 1 < argc && argv[index + 1][0] >= '0' && argv[index + 1][0] <= '9')
				out->capture_after_seconds = (float)atof(argv[++index]);
			continue;
		}
		if (strcmp(argument, "--skip-to-mission") == 0 ||
			strncmp(argument, "--skip-to-mission=", 18) == 0) {
			const char* pilot =
				argument[17] == '=' ? argument + 18 : (index + 1 < argc ? argv[++index] : NULL);
			if (out->skip_to_mission)
				return TieMain_Usage("duplicate option: %s", "--skip-to-mission");
			if (!pilot || !pilot[0])
				return TieMain_Usage("option requires a pilot name: %s", "--skip-to-mission");
			out->skip_to_mission = true;
			out->skip_pilot = pilot;
			continue;
		}
		int option = -1;
		const char* value = NULL;
		for (int candidate = 0; candidate < 3; ++candidate) {
			const size_t length = strlen(names[candidate]);
			if (strcmp(argument, names[candidate]) == 0 ||
				(strncmp(argument, names[candidate], length) == 0 && argument[length] == '=')) {
				option = candidate;
				value = argument[length] == '=' ? argument + length + 1 : NULL;
				break;
			}
		}
		if (option < 0)
			return TieMain_Usage(argument[0] == '-' ? "unknown option: %s"
													: "positional argument is not accepted: %s",
								 argument);
		if (*destinations[option])
			return TieMain_Usage("duplicate option: %s", names[option]);
		if (!value && ++index < argc && strncmp(argv[index], "--", 2) != 0)
			value = argv[index];
		if (!value || !value[0])
			return TieMain_Usage("option requires a directory: %s", names[option]);
		*destinations[option] = value;
	}
	return 0;
}

int main(int argc, char* argv[]) {
	TieLaunchOptions launch;
	const int status = TieMain_ParseOptions(argc, argv, &launch);
	return status ? status : TieApplication_Run(&launch);
}
