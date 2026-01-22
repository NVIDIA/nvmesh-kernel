# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

import os
import sys

# Maximum size for each chunk in bytes (about 0.5MB to be safe)
MAX_CHUNK_SIZE = 500000

def format_chunk(file_path, chunk_number, total_chunks, content):
    """Format a chunk of content with appropriate headers."""
    header = f"\nFile: {file_path} (Part {chunk_number} of {total_chunks})\n"
    return f"{header}\n```c\n{content}\n```\n\n"

def split_content(content, max_size):
    """Split content into chunks of approximately max_size bytes."""
    lines = content.splitlines()
    chunks = []
    current_chunk = []
    current_size = 0

    for line in lines:
        line_size = len(line) + 1  # +1 for newline
        if current_size + line_size > max_size and current_chunk:
            chunks.append('\n'.join(current_chunk))
            current_chunk = []
            current_size = 0
        current_chunk.append(line)
        current_size += line_size

    if current_chunk:
        chunks.append('\n'.join(current_chunk))

    return chunks

def process_file(file_path):
    """Process a single file and return its chunks."""
    try:
        with open(file_path, 'r', encoding='utf-8') as f:
            content = f.read()
            if len(content) > MAX_CHUNK_SIZE:
                chunks = split_content(content, MAX_CHUNK_SIZE)
            else:
                chunks = [content]

            return chunks
    except Exception as e:
        print(f"Error reading {file_path}: {e}", file=sys.stderr)
        return []

def find_c_files(directory):
    """Find all .c and .h files in the given directory and its subdirectories."""
    c_files = []
    for root, _, files in os.walk(directory):
        for file in files:
            if file.endswith(('.c', '.h')):
                c_files.append(os.path.join(root, file))
    return c_files

def write_chunks_to_files(chunks_by_file):
    """Write chunks to numbered output files."""
    chunk_number = 1
    current_output = []
    current_size = 0
    output_files = []

    for file_path, chunks in chunks_by_file.items():
        for i, chunk in enumerate(chunks, 1):
            formatted_chunk = format_chunk(file_path, i, len(chunks), chunk)
            chunk_size = len(formatted_chunk)

            if current_size + chunk_size > MAX_CHUNK_SIZE:
                # Write current chunks to file
                output_file = f"output_{chunk_number}.txt"
                with open(output_file, 'w', encoding='utf-8') as f:
                    f.write(''.join(current_output))
                output_files.append(output_file)
                chunk_number += 1
                current_output = []
                current_size = 0

            current_output.append(formatted_chunk)
            current_size += chunk_size

    # Write remaining chunks
    if current_output:
        output_file = f"output_{chunk_number}.txt"
        with open(output_file, 'w', encoding='utf-8') as f:
            f.write(''.join(current_output))
        output_files.append(output_file)

    return output_files

def main():
    # Use current directory if no path is provided
    project_dir = sys.argv[1] if len(sys.argv) > 1 else '.'

    if not os.path.isdir(project_dir):
        print(f"Error: {project_dir} is not a valid directory", file=sys.stderr)
        sys.exit(1)

    # Find all .c and .h files
    files = find_c_files(project_dir)

    # Process each file and store its chunks
    chunks_by_file = {}
    for file_path in files:
        chunks = process_file(file_path)
        if chunks:
            chunks_by_file[file_path] = chunks

    # Write chunks to output files
    output_files = write_chunks_to_files(chunks_by_file)

    # Print summary
    print("\nFiles have been split into the following chunks:")
    for output_file in output_files:
        print(f"- {output_file}")
    print("\nYou can now upload these files to the chat one at a time.")

if __name__ == "__main__":
    main()
