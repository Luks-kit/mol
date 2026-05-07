/* molc build */

compile(src, obj) {
    auto cmd, cc, cflags;

    cc = "gcc";
    cflags = "-Wall -Wextra -Iinc -g";

    if (newer(src, obj)) {
        printf("  CC ");
        printf(src);
        printf("\n");

        cmd = cat(cc, " ", cflags, " -c ", src, " -o ", obj);
        printf(cmd);
        printf("\n");
        system(cmd);
    }
}

link(objs, out) {
    auto cmd, i, any, cc, ldflags;

    cc = "gcc";

    /* adjust these paths if libdvm.so lives elsewhere */
    ldflags = "-L/usr/local/lib -ldvm -Wl,-rpath,/usr/local/lib -lpthread";

    any = anynewer(objs, out);

    if (any) {
        printf("  LD ");
        printf(out);
        printf("\n");

        cmd = cat(cc, " -o ", out);

        i = 0;
        while (strcmp(objs[i], "") != 0) {
            cmd = cat(cmd, " ", objs[i]);
            i = i + 1;
        }

        cmd = cat(cmd, " ", ldflags);

        printf(cmd);
        printf("\n");

        system(cmd);
    }
}
/* Collect sources from a directory manually */
collect_sources(dir, list, start) {
    auto files, i, j;
    files = glob(cat(dir, "/*.c"));
    i = 0;
    j = start;

    while (strcmp(files[i], "") != 0) {
        list[j] = files[i];
        j = j + 1;
        i = i + 1;
    }

    return j;  /* Return next free index */
}



build_all() {
    auto dirs, sources, objects, i, next;

    /* Allocate arrays with enough room */
    sources = glob("*.c");  /* dummy to get array size */
    objects = glob("*.o");  /* dummy to get array size */

    /* Collect sources from each folder */
    next = 0;
    next = collect_sources("src", sources, next);
    sources[next] = "";

    /* Map sources to build/*.o */
    objects = map_prefix(sources, "src/", "build/");
    objects = map_postfix(objects, ".c", ".o");

    /* Ensure build/ folder exists */
    system("mkdir -p build");

    /* Compile */
    i = 0;
    while (strcmp(sources[i], "") != 0) {
        compile(sources[i], objects[i]);
        i = i + 1;
    }

    /* Link */
    link(objects, "build/molc");
}

clean() {
    printf("Cleaning...\n");
    system("find build/ -type f -delete");
}

install() {
    auto cmd;
    
    build_all();
    cmd = "cp build/molc /usr/local/bin/";
    printf("Instaling...\n");
    system(cmd);

}

main(argc, argv) {
    auto target;

    target = "all";
    if (argc > 1) {
        target = argv[1];
    }

    if (strcmp(target, "all") == 0) {
        build_all();
        return 0;
    }

    if (strcmp(target, "clean") == 0) {
        clean();
        return 0;
    }
    
    if(strcmp(target, "install") == 0) {
        install();
        return 0;
    }


    printf("Unknown target: ");
    printf(target);
    printf("\n");
    printf("Available targets: all, clean, install\n");
    return 1;
}

