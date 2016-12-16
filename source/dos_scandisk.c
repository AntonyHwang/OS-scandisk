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

void update_used_clusters(int usedClusters[], uint16_t cluster, uint32_t size, uint8_t *image_buf, struct bpb33* bpb) {
    
    int total_sectors = size / bpb->bpbBytesPerSec;
    int file_clusters = total_sectors / bpb->bpbSecPerClust;
    int total_clusters = bpb->bpbSectors / bpb->bpbSecPerClust;
    
    printf("size: %i\n", size);
    printf("clusters: %i\n", file_clusters);
    usedClusters[cluster] = 1;
    
    int clusterNum;
    for (clusterNum = 1; clusterNum <= file_clusters; clusterNum++) {
        cluster = get_fat_entry(cluster, image_buf, bpb);
        usedClusters[cluster] = 1;
        printf("cluster no: %i\n", cluster);
    }
}

void follow_dir(int usedClusters[], uint16_t cluster, uint8_t *image_buf, struct bpb33* bpb)
{
    usedClusters[cluster] = 1;
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
                printf("Volume: %s\n", name);
            } else if ((dirent->deAttributes & ATTR_DIRECTORY) != 0) {
                printf("%s (directory)\n\n", name);
                file_cluster = getushort(dirent->deStartCluster);
                follow_dir(usedClusters, file_cluster, image_buf, bpb);
            } else {
                file_cluster = getushort(dirent->deStartCluster);
                size = getulong(dirent->deFileSize);
                printf("%s.%s (%u bytes)\n",
                       name, extension, size);
                update_used_clusters(usedClusters, file_cluster, size, image_buf, bpb);
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
    int clust_size = bpb->bpbSecPerClust * bpb->bpbBytesPerSec;
    int used_clusters[total_clusters];
    printf("total clusters: %i\n", total_clusters);
    
    int i;
    
    for (i = 0; i < total_clusters; i++) {
        used_clusters[i] = 0;
    }
    
    follow_dir(used_clusters, 0, image_buf, bpb);
    
    int shownPrefix = 0;
    for (i = 2; i < total_clusters; i++) {
        if (used_clusters[i] == 0 && get_fat_entry(i, image_buf, bpb) != (FAT12_MASK & CLUST_FREE)) {
            if (!shownPrefix) {
                printf("Unreferenced:");
                shownPrefix = 1;
            }
            printf(" %i", i);
        }
        
        if (i == total_clusters - 1 && shownPrefix) {
            printf("\n");
        }
    }
    
    close(fd);
    exit(0);
}
