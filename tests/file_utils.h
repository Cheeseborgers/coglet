#ifndef COGLET_TESTING_FILE_UTILS
#define COGLET_TESTING_FILE_UTILS

void run_tests(const char *root)
{
    DIR *dir = opendir(root);
    if (!dir)
        return;

    struct dirent *entry;

    while ((entry = readdir(dir))) {

        if (entry->d_name[0] == '.')
            continue;

        char feature_path[PATH_MAX];
        snprintf(feature_path,
                 sizeof(feature_path),
                 "%s/%s",
                 root,
                 entry->d_name);

        run_test_group(feature_path);
    }

    closedir(dir);
}

void run_test_group(const char *group)
{
    run_directory(group, "pass", true);
    run_directory(group, "fail", false);
}

void run_directory(const char *group,
                   const char *subdir,
                   bool should_pass)
{
    char path[PATH_MAX];

    snprintf(path,
             sizeof(path),
             "%s/%s",
             group,
             subdir);

    DIR *dir = opendir(path);
    if (!dir)
        return;

    struct dirent *entry;

    while ((entry = readdir(dir))) {

        if (entry->d_name[0] == '.')
            continue;

        if (!ends_with(entry->d_name, ".cog"))
            continue;

        char filename[PATH_MAX];

        snprintf(filename,
                 sizeof(filename),
                 "%s/%s",
                 path,
                 entry->d_name);

        run_single_test(filename, should_pass);
    }

    closedir(dir);
}

#endif