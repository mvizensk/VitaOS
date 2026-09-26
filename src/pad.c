/* vita-elf-create puts the import tables (~4 KB) between the code segment and
 * the data segment, which starts at the next 64 KB boundary. When the code
 * happens to end in the last ~4 KB of a 64 KB page, packaging fails ("Cannot
 * allocate ... for SCE data at end of segment 0; segment 1 overlaps"). This
 * block moves the boundary; if the error comes back, change its size. */
__attribute__((used)) const unsigned char segment_pad[6 * 1024] = {1};
