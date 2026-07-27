awk '
function print_link() {
    if (source) {
        # Ensure the "dist/pytools/" directory exists
        system("mkdir -p dist/pytools");
        cmd = "ln -s " (linkpath ? linkpath : (linkdir "/" source)) " dist/pytools/" tool;
        print cmd;
        system(cmd);  # Execute the command to create the symbolic link
    }
    tool=""; source=""; linkdir=""; linkpath="";
}
/^ *[^ ]+: *$/ { print_link(); tool = $1; sub(":", "", tool); }
/^ *source: */ { source = $2; }
/^ *linkdir: */ { linkdir = $2; }
/^ *linkpath: */ { linkpath = $2; }
END             { print_link(); }
' tools.yaml
