#!/bin/bash

# Get the directory and name of the current script
script_dir=$(dirname "${0}")
script_dir=$( (
  cd "${script_dir}" || errorexit 1 "Error: could not change directory to ${script_dir}"
  pwd
))
script_name=$(basename "${0}")

set -e

# Function for error handling and exit
function errorexit() {
  error_code="${1}"
  shift
  echo "$@"
  exit "${error_code}"
}

# Function to display usage information
function usage() {
  echo "USAGE ${script_name} <tostrip>"
}

# Break down the input argument into directory and filename
input_dir=$(dirname "$1")
input_file=$(basename "$1")

# Check if filename is specified
if [[ -z "${input_file}" ]]; then
  usage
  errorexit 0 "Error: tostrip must be specified"
fi

cd "${input_dir}" || errorexit 1 "Error: could not change directory to ${input_dir}"

# Define debug directory and file
debug_dir=.debug
debug_file="${input_file}.debug"

# Create debug directory if it does not exist
if [[ ! -d "${debug_dir}" ]]; then
  echo "Creating directory ${input_dir}/${debug_dir}"
  mkdir -p "${debug_dir}"
fi

# Strip the file and put debug info into debug file
echo "Stripping ${input_file}, putting debug info into ${debug_file}"
objcopy --only-keep-debug "${input_file}" "${debug_dir}/${debug_file}"
strip --strip-debug --strip-unneeded "${input_file}"
objcopy --add-gnu-debuglink="${debug_dir}/${debug_file}" "${input_file}"

# Remove executable permission from debug file
chmod -x "${debug_dir}/${debug_file}"
