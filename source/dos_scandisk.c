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

uint32_t get_file_length(uint16_t cluster, uint8_t *image_buf, struct bpb33* bpb)
{
    uint32_t length = 1;
    
    cluster = get_fat_entry(cluster, image_buf, bpb);
    while (!is_end_of_file(cluster)) {
        cluster = get_fat_entry(cluster, image_buf, bpb);
        length++;
    }
    
    return length;
}

void mark_file_clusters_used(int usedClusters[], uint16_t cluster, uint32_t bytes_remaining, uint8_t *image_buf, struct bpb33* bpb)
{
    usedClusters[cluster] = 1;
    
    int clust_size = bpb->bpbSecPerClust * bpb->bpbBytesPerSec;
    int total_clusters = bpb->bpbSectors / bpb->bpbSecPerClust;
    
    if (cluster == 0) {
        fprintf(stderr, "Bad file termination\n");
        return;
    } else if (cluster > total_clusters) {
        abort(); /* this shouldn't be able to happen */
    }
    
    uint16_t next_cluster = get_fat_entry(cluster, image_buf, bpb);
    
    if (is_end_of_file(next_cluster)) {
        return;
    } else {
        mark_file_clusters_used(usedClusters, get_fat_entry(cluster, image_buf, bpb), bytes_remaining - clust_size, image_buf, bpb);
    }
}

void check_lost_files(int usedClusters[], uint16_t cluster, uint8_t *image_buf, struct bpb33* bpb)
{
    // A value of 1 means that the cluster is used somewhere.
    usedClusters[cluster] = 1;
    
    struct direntry *dirent;
    int d, i;
    dirent = (struct direntry*) cluster_to_addr(cluster, image_buf, bpb);
    int clust_size = bpb->bpbBytesPerSec * bpb->bpbSecPerClust;
    
    while (1) {
        for (d = 0; d < clust_size; d += sizeof(struct direntry)) {
            char name[9];
            char extension[4];
            uint32_t size;
            uint16_t file_cluster;
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
            if (strcmp(name, ".") == 0) {
                dirent++;
                continue;
            }
            if (strcmp(name, "..") == 0) {
                dirent++;
                continue;
            }
            
            if ((dirent->deAttributes & ATTR_VOLUME) != 0) {
            } else if ((dirent->deAttributes & ATTR_DIRECTORY) != 0) {
                file_cluster = getushort(dirent->deStartCluster);
                //check_lost_files(usedClusters, file_cluster, image_buf, bpb);
            } else {
                /* We have a file. We should follow the file and remove all the used clusters from our collection! */
                size = getulong(dirent->deFileSize);
                uint16_t file_cluster_begin = getushort(dirent->deStartCluster);
                //uint16_t cluster, uint32_t bytes_remaining, uint8_t *image_buf, struct bpb33* bpb
                mark_file_clusters_used(usedClusters, file_cluster_begin, size, image_buf, bpb);
            }
            
            dirent++;
        }
        
        /* We've reached the end of the cluster for this directory. Where's the next cluster? */
        if (cluster == 0) {
            // root dir is special
            dirent++;
        } else {
            cluster = get_fat_entry(cluster, image_buf, bpb);
            dirent = (struct direntry*) cluster_to_addr(cluster, image_buf, bpb);
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
    check_lost_files(used_clusters, 0, image_buf, bpb);
    
    int i;
    
    printf("Unreferenced:");
    for (i = 2; i < total_clusters; i++) {
        if (used_clusters[i] == 0 && get_fat_entry(i, image_buf, bpb) != CLUST_FREE) {
            printf(" %i", i);
        }
    }
    /*for (i = 2; i < total_clusters; i++) {
        if (used_clusters[i] == 0 && get_fat_entry(i, image_buf, bpb) != CLUST_FREE) {
            if (!shownPrefix) {
                printf("Lost File: ");
            }
            
            uint16_t size = get_file_length(i, image_buf, bpb);
            printf("%i %i\n", i, size);
            
            struct direntry *dirent = (struct direntry*) cluster_to_addr(0, image_buf, bpb);
            uint32_t size_bytes = size * clust_size;
            
            const char base[] = "found";
            const char extension[] = ".dat";
            char filename [13];
            sprintf(filename, "%s%i%s", base, foundCount++, extension);
            
            create_dirent(dirent, filename, i, size_bytes, image_buf, bpb);
            
            check_lost_files(used_clusters, 0, image_buf, bpb);
        }
        
        if (i == total_clusters - 1 && shownPrefix) {
            printf("\n");
        }
    }*/
    
    //check_file_length(0, image_buf, bpb);
    
    close(fd);
    exit(0);
}
