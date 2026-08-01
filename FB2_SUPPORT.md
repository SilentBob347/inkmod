# FB2 support

The file browser accepts `.fb2` and `.fb2.zip` files.

For a regular FB2, the reader creates an unpacked EPUB package in
`/.inkmod`.  The EPUB reader now reads that package directly.  For an
`fb2.zip` archive, the first embedded `.fb2` file is extracted to a temporary
file, converted, and the temporary file is removed afterwards.

The generated package is a cache.  It can be safely removed through the book
actions menu or by deleting its corresponding `/.inkmod/epub_*` folder.
