esptesptool.py --chip esp32s3 merge_bin -o merged_firmware.bin \
    --flash_mode dio --flash_freq 80m --flash_size 2MB \
    0x0 bootloader/bootloader.bin \
    0x8000 partition_table/partition-table.bin \
    0x10000 SPAIA_field_agent.bin