#include "ui.h"

int main(int argc, char **argv)
{
    GtkApplication *app;
    int exit_code;

    app = odysia_create_application();
    exit_code = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);

    return exit_code;
}