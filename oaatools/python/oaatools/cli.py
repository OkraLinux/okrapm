#!/usr/bin/env python3
import sys
import argparse
from oaatools import OaaBuilder, OaaMetadata

def main():
    parser = argparse.ArgumentParser(description="oaatools - Python CLI Engine for Okra Application Artifacts")
    subparsers = parser.add_subparsers(dest="command", required=True)

    # new
    p_new = subparsers.add_parser("new", help="Create a package skeleton")
    p_new.add_argument("dir", help="Directory path")
    p_new.add_argument("--name", default="app-demo", help="Package name")
    p_new.add_argument("--version", default="1.0.0", help="Package version")
    p_new.add_argument("--namespace", default="app", help="Package namespace")

    # build
    p_build = subparsers.add_parser("build", help="Build .oaa package from directory")
    p_build.add_argument("dir", help="Source directory path")
    p_build.add_argument("-o", "--output", help="Output .oaa file path")

    # inspect
    p_inspect = subparsers.add_parser("inspect", help="Inspect .oaa package metadata")
    p_inspect.add_argument("file", help="Path to .oaa file")

    # extract
    p_extract = subparsers.add_parser("extract", help="Extract .oaa package to directory")
    p_extract.add_argument("file", help="Path to .oaa file")
    p_extract.add_argument("dest", help="Destination directory")

    args = parser.parse_args()

    if args.command == "build":
        try:
            out = OaaBuilder.build(args.dir, args.output)
            print(f":: Package built: {out}")
        except Exception as e:
            print(f"Error: {e}", file=sys.stderr)
            sys.exit(1)
    elif args.command == "inspect":
        meta = OaaBuilder.inspect(args.file)
        if not meta:
            print(f"Error inspecting {args.file}", file=sys.stderr)
            sys.exit(1)
        print(f"Artifact:     {args.file}")
        print(f"Name:         {meta.name}")
        print(f"Namespace:    {meta.namespace}")
        print(f"Version:      {meta.version}")
        print(f"Arch:         {meta.architecture}")
        print(f"Description:  {meta.description}")
        if meta.dependencies:
            print(f"Dependencies: {' '.join(meta.dependencies)}")
    elif args.command == "extract":
        if OaaBuilder.extract(args.file, args.dest):
            print(f":: Extracted {args.file} to {args.dest}")
        else:
            print(f"Error extracting {args.file}", file=sys.stderr)
            sys.exit(1)

if __name__ == "__main__":
    main()
