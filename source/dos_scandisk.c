#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <string.h>
#include <ctype.h>

#include "bootsect.h"
#include "bpb.h"
#include "direntry.h"
#include "fat.h"
#include "dos.h"

int flag = 0;

void assign_used_clusters(int nonEmptyClusters[], uint16_t cluster, uint32_t size, uint8_t *image_buf, struct bpb33* bpb)
{
    nonEmptyClusters[cluster] = 1;
    
    while (1) {
        cluster = get_fat_entry(cluster, image_buf, bpb);
        if (is_end_of_file(cluster)) {
            break;
        }
        else {
            nonEmptyClusters[cluster] = 1;
        }
    }
}

int get_file_blocks(uint16_t cluster, uint8_t *image_buf, struct bpb33* bpb) {
    int blocks = 0;
    while (1) {
        cluster = get_fat_entry(cluster, image_buf, bpb);
        if (is_end_of_file(cluster)) {
            blocks++;
            return blocks;
        }
        else {
            blocks++;
        }
    }
}

int check_file_size(uint16_t cluster, uint32_t size, uint8_t *image_buf, struct bpb33* bpb) {
    uint32_t BytesPerBlock = bpb->bpbBytesPerSec * bpb->bpbSecPerClust;
    uint32_t fat_file_blocks = get_file_blocks(cluster, image_buf, bpb);
    uint32_t fat_file_size = fat_file_blocks * BytesPerBlock;
    uint32_t dirent_blocks;
    
    dirent_blocks = (size + BytesPerBlock - 1) / BytesPerBlock;
    
    if (fat_file_blocks != dirent_blocks) {
        uint16_t startCluster = cluster + BytesPerBlock - 1;
        uint16_t endCluster = cluster + fat_file_blocks;
        uint16_t current_cluster = startCluster;
        while(1) {
            uint16_t next_cluster = get_fat_entry(current_cluster, image_buf, bpb);
            set_fat_entry(current_cluster, FAT12_MASK&CLUST_FREE, image_buf, bpb);
            if (current_cluster == endCluster || is_end_of_file(next_cluster)) {
                break;
            }
            current_cluster = next_cluster;
        }
        set_fat_entry(startCluster, FAT12_MASK&CLUST_EOFS, image_buf, bpb);
        return fat_file_size;
    }
    else {
        return 0;
    }
}

void follow_dir(int check, int nonEmptyClusters[], uint16_t cluster, uint8_t *image_buf, struct bpb33* bpb)
{
    nonEmptyClusters[cluster] = 1;
    struct direntry *dirent;
    int d, i;
    dirent = (struct direntry*)cluster_to_addr(cluster, image_buf, bpb);
    while (1) {
        for (d = 0; d < bpb->bpbBytesPerSec * bpb->bpbSecPerClust;
             d += sizeof(struct direntry)) {
            char name[9];
            char extension[4];
            uint32_t size;
            uint16_t file_cluster = 0;
            name[8] = ' ';
            extension[3] = ' ';
            memcpy(name, &(dirent->deName[0]), 8);
            memcpy(extension, dirent->deExtension, 3);
            if (name[0] == SLOT_EMPTY)
                return;
            
            /* skip over deleted entries */
            if (((uint8_t)name[0]) == SLOT_DELETED)
                continue;
            
            /* names are space padded - remove the spaces */
            for (i = 8; i > 0; i--) {
                if (name[i] == ' ')
                    name[i] = '\0';
                else
                    break;
            }
            
            /* remove the spaces from extensions */
            for (i = 3; i > 0; i--) {
                if (extension[i] == ' ')
                    extension[i] = '\0';
                else
                    break;
            }
            
            /* don't print "." or ".." directories */
            if (strcmp(name, ".")==0) {
                dirent++;
                continue;
            }
            if (strcmp(name, "..")==0) {
                dirent++;
                continue;
            }
            
            if ((dirent->deAttributes & ATTR_VOLUME) != 0) {
            } else if ((dirent->deAttributes & ATTR_DIRECTORY) != 0) {
                file_cluster = getushort(dirent->deStartCluster);
                follow_dir(check, nonEmptyClusters, file_cluster, image_buf, bpb);
            } else {
                file_cluster = getushort(dirent->deStartCluster);
                size = getulong(dirent->deFileSize);
                if (check == 0) {
                    assign_used_clusters(nonEmptyClusters, file_cluster, size, image_buf, bpb);
                }
                else if (check == 1) {
                    int file_size = check_file_size(file_cluster, size, image_buf, bpb);
                    if (file_size != 0) {
                        printf("%s.%s %i %i\n", name, extension, size, file_size);
                    }
                }
            }
            dirent++;
        }
        if (cluster == 0) {
            // root dir is special
            dirent++;
        } else {
            cluster = get_fat_entry(cluster, image_buf, bpb);
            dirent = (struct direntry*)cluster_to_addr(cluster, 
                                                       image_buf, bpb);
        }
    }
}

void usage()
{
    fprintf(stderr, "Usage: dos_scandisk <imagename>\n");
    exit(1);
}



void find_unrefClusters(int nonEmptyClusters[], int total_clusters, uint8_t *image_buf, struct bpb33* bpb)
{
    int printed = 0;
    
    int cluster;
    for (cluster = 0; cluster < total_clusters; cluster++) {
        nonEmptyClusters[cluster] = 0;
    }
    
    follow_dir(0, nonEmptyClusters, 0, image_buf, bpb);
    
    for (cluster = 2; cluster < total_clusters; cluster++) {
        if (nonEmptyClusters[cluster] == 0 && get_fat_entry(cluster, image_buf, bpb) != (FAT12_MASK & CLUST_FREE)) {
            if (printed == 0) {
                printf("Unreferenced:");
                printed = 1;
            }
            flag = 1;
            printf(" %i", cluster);
        }
    }
    if (flag == 1) {
        printf("\n");
    }
}

void write_dirent(struct direntry *dirent, char *filename,
                  uint16_t start_cluster, uint32_t size)
{
    char *p, *p2;
    char *uppername;
    int len, i;
    
    /* clean out anything old that used to be here */
    memset(dirent, 0, sizeof(struct direntry));
    
    /* extract just the filename part */
    uppername = strdup(filename);
    p2 = uppername;
    for (i = 0; i < strlen(filename); i++) {
        if (p2[i] == '/' || p2[i] == '\\') {
            uppername = p2+i+1;
        }
    }
    
    /* convert filename to upper case */
    for (i = 0; i < strlen(uppername); i++) {
        uppername[i] = toupper(uppername[i]);
    }
    
    /* set the file name and extension */
    memset(dirent->deName, ' ', 8);
    p = strchr(uppername, '.');
    memcpy(dirent->deExtension, "___", 3);
    if (p == NULL) {
        fprintf(stderr, "No filename extension given - defaulting to .___\n");
    } else {
        *p = '\0';
        p++;
        len = strlen(p);
        if (len > 3) len = 3;
        memcpy(dirent->deExtension, p, len);
    }
    if (strlen(uppername)>8) {
        uppername[8]='\0';
    }
    memcpy(dirent->deName, uppername, strlen(uppername));
    free(p2);
    
    /* set the attributes and file size */
    dirent->deAttributes = ATTR_NORMAL;
    putushort(dirent->deStartCluster, start_cluster);
    putulong(dirent->deFileSize, size);
    
    /* a real filesystem would set the time and date here, but it's
     not necessary for this coursework */
}

void create_unref_dirent(char *filename, uint16_t start_cluster, uint32_t size, uint8_t *image_buf, struct bpb33* bpb) {
    struct direntry *dirent = (struct direntry*) cluster_to_addr(0, image_buf, bpb);

    while(1) {
        if (dirent->deName[0] == SLOT_EMPTY) {
            /* we found an empty slot at the end of the directory */
            write_dirent(dirent, filename, start_cluster, size);
            dirent++;
            
            /* make sure the next dirent is set to be empty, just in
             case it wasn't before */
            memset((uint8_t*)dirent, 0, sizeof(struct direntry));
            dirent->deName[0] = SLOT_EMPTY;
            return;
        }
        if (dirent->deName[0] == SLOT_DELETED) {
            /* we found a deleted entry - we can just overwrite it */
            write_dirent(dirent, filename, start_cluster, size);
            return;
        }
        dirent++;
    }
}

void get_lost_files(int nonEmptyClusters[], int total_clusters, uint8_t *image_buf, struct bpb33* bpb)
{
    int clust_size = bpb->bpbSecPerClust * bpb->bpbBytesPerSec;
    int fileFound = 0;
    int cluster;
    for (cluster = 2; cluster < total_clusters; cluster++) {
        if (nonEmptyClusters[cluster] == 0 && get_fat_entry(cluster, image_buf, bpb) != (FAT12_MASK & CLUST_FREE)) {
            int start_cluster = cluster;
            uint16_t blocks = get_file_blocks(cluster, image_buf, bpb);
            printf("Lost File: %i %i\n", start_cluster, blocks);
            fileFound++;
            int size = blocks * clust_size;
            
            char filename [13];
            sprintf(filename, "found%i.dat", fileFound);
            
            create_unref_dirent(filename, cluster, size, image_buf, bpb);
            
            follow_dir(0, nonEmptyClusters, 0, image_buf, bpb);
        }
    }
}

int main(int argc, char** argv)
{
    uint8_t *image_buf;
    int fd;
    struct bpb33* bpb;
    if (argc != 2) {
        usage();
    }
    
    image_buf = mmap_file(argv[1], &fd);
    bpb = check_bootsector(image_buf);
    
    int total_clusters = bpb->bpbSectors / bpb->bpbSecPerClust;
    int nonEmptyClusters[total_clusters];
    //get unreferenced clusters
    find_unrefClusters(nonEmptyClusters, total_clusters, image_buf, bpb);
    if (flag == 1) {
        //get number of blocks
        get_lost_files(nonEmptyClusters, total_clusters, image_buf, bpb);
        //print inconsistent file names
        follow_dir(1, nonEmptyClusters, 0, image_buf, bpb);
    }
    

    
    close(fd);
    exit(0);
}
