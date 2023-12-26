#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>
#include <assert.h>

#include "gguflib.h"
#include "sds.h"

/* ========================== Utility functions  ============================ */

/* Glob-style pattern matching. Return 1 on match, 0 otherwise. */
int strmatch(const char *pattern, int patternLen,
             const char *string, int stringLen, int nocase)
{
    while(patternLen && stringLen) {
        switch(pattern[0]) {
        case '*':
            while (patternLen && pattern[1] == '*') {
                pattern++;
                patternLen--;
            }
            if (patternLen == 1)
                return 1; /* match */
            while(stringLen) {
                if (strmatch(pattern+1, patternLen-1,
                             string, stringLen, nocase))
                    return 1; /* match */
                string++;
                stringLen--;
            }
            return 0; /* no match */
            break;
        case '?':
            string++;
            stringLen--;
            break;
        case '[':
        {
            int not, match;

            pattern++;
            patternLen--;
            not = pattern[0] == '^';
            if (not) {
                pattern++;
                patternLen--;
            }
            match = 0;
            while(1) {
                if (pattern[0] == '\\' && patternLen >= 2) {
                    pattern++;
                    patternLen--;
                    if (pattern[0] == string[0])
                        match = 1;
                } else if (pattern[0] == ']') {
                    break;
                } else if (patternLen == 0) {
                    pattern--;
                    patternLen++;
                    break;
                } else if (patternLen >= 3 && pattern[1] == '-') {
                    int start = pattern[0];
                    int end = pattern[2];
                    int c = string[0];
                    if (start > end) {
                        int t = start;
                        start = end;
                        end = t;
                    }
                    if (nocase) {
                        start = tolower(start);
                        end = tolower(end);
                        c = tolower(c);
                    }
                    pattern += 2;
                    patternLen -= 2;
                    if (c >= start && c <= end)
                        match = 1;
                } else {
                    if (!nocase) {
                        if (pattern[0] == string[0])
                            match = 1;
                    } else {
                        if (tolower((int)pattern[0]) == tolower((int)string[0]))
                            match = 1;
                    }
                }
                pattern++;
                patternLen--;
            }
            if (not)
                match = !match;
            if (!match)
                return 0; /* no match */
            string++;
            stringLen--;
            break;
        }
        case '\\':
            if (patternLen >= 2) {
                pattern++;
                patternLen--;
            }
            /* fall through */
        default:
            if (!nocase) {
                if (pattern[0] != string[0])
                    return 0; /* no match */
            } else {
                if (tolower((int)pattern[0]) != tolower((int)string[0]))
                    return 0; /* no match */
            }
            string++;
            stringLen--;
            break;
        }
        pattern++;
        patternLen--;
        if (stringLen == 0) {
            while(*pattern == '*') {
                pattern++;
                patternLen--;
            }
            break;
        }
    }
    if (patternLen == 0 && stringLen == 0)
        return 1;
    return 0;
}

/* ========================== 'show' subcommand ============================= */

void gguf_tools_show(const char *filename) {
    gguf_ctx *ctx = gguf_init(filename);
    if (ctx == NULL) {
        perror("Opening GGUF file");
        exit(1);
    }

    /* Show general information about the neural network. */
    printf("%s (ver %d): %llu key-value pairs, %llu tensors\n",
        filename,
        (int)ctx->header->version,
        (unsigned long long)ctx->header->metadata_kv_count,
        (unsigned long long)ctx->header->tensor_count);

    /* Show all the key-value pairs. */
    gguf_key key;
    while (gguf_get_key(ctx,&key)) {
        printf("%.*s: [%s] ", (int)key.namelen, key.name, gguf_get_value_type_name(key.type));
        gguf_print_value(ctx,key.type,key.val,0);
        printf("\n");
    }

    /* Show all the tensors. */
    gguf_tensor tensor;
    while (gguf_get_tensor(ctx,&tensor)) {
        printf("%s tensor %.*s @%llu, %llu weights, %llu bytes\n",
            gguf_get_tensor_type_name(tensor.type),
            (int)tensor.namelen,
            tensor.name,
            tensor.offset,
            tensor.num_weights,
            tensor.bsize);
    }
    return;
}

/* Read a Mixtral MoE model and creates a new non-MoE GGUF file based
 * on the weights of the expert with id 'expert_id'. */
void gguf_tools_split_mixtral(int expert_id, const char *mixtral_filename, const char *output_filename) {
    gguf_ctx *mixtral = gguf_init(mixtral_filename);
    if (mixtral == NULL) {
        perror("Opening Mixtral file");
        exit(1);
    }

    gguf_ctx *output = gguf_create(output_filename);
    if (output == NULL) {
        perror("Opening the output file");
        exit(1);
    }

    /* To start, copy all the key value items, excluding the one
     * related to the experts. */
    gguf_key key;
    while (gguf_get_key(mixtral,&key)) {
        char keybuf[1024];
        snprintf(keybuf,sizeof(keybuf),"%.*s",(int)key.namelen, key.name);

        int skip = strstr(keybuf,"llama.expert_") != NULL;

        if (!skip)
            printf("Copying %s\n", keybuf);
        uint64_t value_start_offset = mixtral->off;
        void *value = mixtral->data+mixtral->off;
        // Just consume the value without doing anything with it.
        gguf_do_with_value(mixtral,key.type,key.val,NULL,0,0,NULL);
        uint64_t value_len = mixtral->off - value_start_offset;

        // Now append the value to the output model.
        if (!skip)
            gguf_append_kv(output,key.name,key.namelen,key.type,value,value_len);
    }

    /* Now it's time to copy the tensors. We need to copy all the shared
     * tensors (between the different experts), but only a set of
     * expert-specific tensors corresponding to the expert ID the user
     * wants to extract. */
    struct tensor_to_copy {
        sds dest_name;          // Tensor name in the output file.
        gguf_tensor orig_info;  // Original tensor info.
        uint64_t dest_offset;   // Destination offset in output file.
        uint64_t size;          // Tensor total bytes.
    };

    uint32_t num_tensors = 0;
    uint32_t max_tensors = 2048;

    struct tensor_to_copy *tensors =
        malloc(sizeof(struct tensor_to_copy)*max_tensors);
    if (tensors == NULL) {
        perror("Allocating tensors info array");
        exit(1);
    }

    /* Scan Mixtral tensors looking for the ones we need to copy
     * in the output model. */
    gguf_tensor tensor_info;
    while (gguf_get_tensor(mixtral,&tensor_info)) {
        assert(num_tensors < max_tensors);

        char tn[1024]; // Tensor name as null terminated string.
        snprintf(tn,sizeof(tn),"%.*s",(int)tensor_info.namelen, tensor_info.name);

        /* The tensor is a feed-forward tensor? We want to copy only
         * the ones of our expert ID. */
        if (strstr(tn,".ffn_") != NULL && strstr(tn,".ffn_norm") == NULL) {
            char match[32];
            snprintf(match,sizeof(match),".%d.weight",expert_id);
            char *match_ptr = strstr(tn,match);
            if (match_ptr == NULL) {
                printf("Skipping tensor %s\n", tn);
                continue; // Skip this tensor.
            }

            /* We need to remove the .<id>. from the name. */
            size_t taillen = strlen(match_ptr);
            memmove(match_ptr,match_ptr+2,taillen+1);
        }

        /* Create the entry for this tensor. Later we will scan all our
         * entries and append data to our output tensor. */
        tensors[num_tensors].dest_name = sdsnew(tn);
        if (tensors[num_tensors].dest_name == NULL) {
            perror("Allocating test tensor name");
            exit(1);
        }
        tensors[num_tensors].orig_info = tensor_info;
        tensors[num_tensors].size = tensor_info.bsize;
        num_tensors++;
    }

    /* Now we need to set the offset for our destination tensors. As
     * we calculate the offsets, we can emit the tensors information
     * section as well. */
    uint64_t tensor_off = 0; // Tensor offsets are relative to data section,
                             // so we start at offset 0.
    for (uint32_t j = 0; j < num_tensors; j++) {
        /* Align offset. */
        tensor_off += gguf_get_alignment_padding(mixtral->alignment,tensor_off);
        tensors[j].dest_offset = tensor_off;
        if (gguf_append_tensor_info(output,tensors[j].dest_name,strlen(tensors[j].dest_name),tensors[j].orig_info.ndim,tensors[j].orig_info.dim,tensors[j].orig_info.type,tensor_off) == 0)
        {
            perror("Failed to append tensor info");
            exit(1);
        }
        tensor_off += tensors[j].orig_info.bsize;
    }
    printf("Output file: after writing tensors info, file size is: %llu\n", output->size);

    /* Finally, append the tensors weights. */
    for (uint32_t j = 0; j < num_tensors; j++) {
        printf("Writing tensor %s\n", tensors[j].dest_name);
        if (gguf_append_tensor_data(output,tensors[j].orig_info.weights_data,
            tensors[j].orig_info.bsize) == 0)
        {
            perror("Failed to append tensor data");
            exit(1);
        }
    }
    exit(0);
}

/* ======================= Main and CLI options parsing ===================== */

void gguf_tools_usage(const char *progname) {
    printf("Usage: %s <subcommand> [options...]\n"
           "Subcommands:\n"
           "  show <filename> -- show GGUF model keys and tensors.\n"
           "  split-mixtral <id> mixtral.gguf out.gguf -- extract expert.\n"
           , progname);
    exit(1);
}

int main(int argc, char **argv) {
    if (argc < 3) gguf_tools_usage(argv[0]);

    if (!strcmp(argv[1],"show") && argc == 3) {
        gguf_tools_show(argv[2]);
    } else if (!strcmp(argv[1],"split-mixtral") && argc == 5) {
        gguf_tools_split_mixtral(atoi(argv[2]),argv[3],argv[4]);
    } else {
        gguf_tools_usage(argv[0]);
    }
    return 0;
}
