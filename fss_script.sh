#!/bin/bash

RED='\033[0;31m'
GREEN='\033[0;32m'
NC='\033[0m'

if [[ $# -ne 4 ]]; then
    echo -e "${RED}Usage:${NC} $0 -p <path> -c <command>"
    exit 1
fi

while [[ $# -gt 0 ]]; do
    key="$1"
    case $key in
        -p)
        PATH_ARG="$2"
        shift 2
        ;;
        -c)
        CMD="$2"
        shift 2
        ;;
        *)
        echo -e "${RED}Unknown option $1${NC}"
        exit 1
        ;;
    esac
done

if [[ -d "$PATH_ARG" ]]; then
    if [[ "$CMD" != "purge" ]]; then
        echo -e "${RED}Error:${NC} When path is a directory, only 'purge' command is allowed."
        exit 1
    fi
elif [[ ! -f "$PATH_ARG" ]]; then
    echo -e "${RED}Error:${NC} Path '$PATH_ARG' does not exist or is not a valid file."
    exit 1
fi

case "$CMD" in
    listAll)
        echo -e "${GREEN}Listing all directories:${NC}"
        # Associative arrays
        declare -A seen_src 
        declare -A last_ts
        declare -A arr_target
        declare -A arr_status

        # Extract all lines matching the format [] [] ... [] where the last bracket contains SUCCESS, ERROR, or PARTIAL
        while read -r line; do
            # grep -oP extracts only the brackets [], sed -n 2p/3p selects the specific match, and tr -d '[]' removes the brackets
            src=$(echo "$line" | grep -oP '\[.*?\]' | sed -n 2p | tr -d '[]')
            tgt=$(echo "$line" | grep -oP '\[.*?\]' | sed -n 3p | tr -d '[]')
            # cut splits the line at the first closing bracket ']' to extract the timestamp
            ts=$(echo "$line" | cut -d']' -f1 | tr -d '[')
            status=$(echo "$line" | grep -oP '\[(SUCCESS|ERROR|PARTIAL)\]' | tr -d '[]')
            
            ts_unix=$(date -d "$ts" +%s)
            last_ts_unix=$(date -d "${last_ts[$src]}" +%s 2>/dev/null || echo 0)

            # Returns '_' if the key exists, otherwise returns nothing. 
            # The -z flag checks if the returned string is empty.
            if [[ -z "${seen_src[$src]+_}"  || "$ts_unix" -gt "last_ts_unix" ]]; then
                seen_src[$src]=1
                last_ts[$src]="$ts"
                arr_target[$src]="$tgt"
                arr_status[$src]="$status"
            fi
        done < <(grep -E '\[.*\] \[.*\] \[.*\] \[.*\] \[(SUCCESS|ERROR|PARTIAL)\]' "$PATH_ARG")

        for src in "${!last_ts[@]}"; do
            echo "$src -> ${arr_target[$src]} [Last Sync: ${last_ts[$src]}] [${arr_status[$src]}]"
        done | sort -u
        ;;

    listMonitored)
        echo -e "${GREEN}Listing monitored directories:${NC}"
        # Find lines starting with "Monitoring started for" or "Syncing directory:" 
        grep "Monitoring started for\|Syncing directory:" "$PATH_ARG" | while read -r line; do
            if echo "$line" | grep -q "Monitoring started for"; then
                dir=$(echo "$line" | cut -d' ' -f6) # cut -d' ' splits the line by spaces and extracts the 6th field (source_dir)
            else
                dir=$(echo "$line" | cut -d' ' -f5 | cut -d' ' -f6)
            fi

            # Check if a "Monitoring stopped" event occurred after the last "Monitoring started" or "Syncing" event
            # -n outputs the line number, tail -1 keeps the last occurrence, cut -d: -f1 extracts just the line number
            # Store the line numbers of the last start and stop events (if any)
            last_start=$(grep -n "Monitoring started for $dir\|Syncing directory: $dir" "$PATH_ARG" | tail -1 | cut -d: -f1)
            last_stop=$(grep -n "Monitoring stopped for $dir" "$PATH_ARG" | tail -1 | cut -d: -f1 2>/dev/null) # Will be empty if not found
            
            # -z checks for emptiness; otherwise, it checks if the stop occurred before (-lt) the start of monitoring
            if [[ -z "$last_stop" || $last_stop -lt $last_start ]]; then
                last_status_line=$(grep "\[$dir\]" "$PATH_ARG" | tail -1)
                tgt=$(echo "$last_status_line" | grep -oP '\[.*?\]' | sed -n 2p | tr -d '[]')
                ts=$(echo "$last_status_line" | cut -d']' -f1 | tr -d '[')
                echo "$dir -> $tgt [Last Sync: $ts]"
            fi
        done | sort -u
        ;;

    listStopped)
        echo -e "${GREEN}Listing stopped directories:${NC}"
        grep "Monitoring stopped for" "$PATH_ARG" | cut -d' ' -f6- | sort | uniq | while read -r dir; do
            last_stop=$(grep -n "Monitoring stopped for $dir" "$PATH_ARG" | tail -1 | cut -d: -f1)
            last_start=$(grep -n "Monitoring started for $dir\|Syncing directory: $dir" "$PATH_ARG" | tail -1 | cut -d: -f1 2>/dev/null)
            if [[ -z "$last_start" || $last_start -lt $last_stop ]]; then
                last_status_line=$(grep "\[$dir\]" "$PATH_ARG" | tail -1)
                tgt=$(echo "$last_status_line" | grep -oP '\[.*?\]' | sed -n 3p | tr -d '[]')
                ts=$(echo "$last_status_line" | cut -d']' -f1 | tr -d '[')
                echo "$dir -> $tgt [Last Sync: $ts]"
            fi
        done | sort -u
        ;;

    purge)
        echo -e "${RED}Deleting '$PATH_ARG'...${NC}"
        rm -rf "$PATH_ARG"
        echo -e "${GREEN}Purge complete.${NC}"
        ;;

    *)
        echo -e "${RED}Invalid command: $CMD${NC}"
        exit 1
        ;;
esac
