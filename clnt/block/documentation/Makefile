# Pandoc settings
PANDOC = pandoc

# Input and output files
MD_FILES = clients_block_device_overview.md block_feature_matrix.md
HTML_FILES = $(MD_FILES:.md=.html)

# Default target: build PDFs
all: $(HTML_FILES)

# Rule: .md -> .html
%.html: %.md
	$(PANDOC) $< -o $@

# Clean up generated files
clean:
	rm -f $(HTML_FILES)
