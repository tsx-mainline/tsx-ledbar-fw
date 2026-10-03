# Update protocol

The LED bar takes a new application image through its own bootloader. The transport is the Cresnet packet framing on USB, the same framing that the joins use. This page describes the host side. `tools/tsx-ledbar-flash` implements it.

## Framing

Every packet is one USB bulk transfer:

| Byte | Meaning |
| --- | --- |
| 0 | destination: `02` for every update packet (the joins use `00`) |
| 1 | length: the number of bytes after this byte |
| 2 | packet type |
| 3.. | payload |

The bar answers with packets of the same shape.

## Modes and endpoints

| Mode | USB id | Interface | Endpoints |
| --- | --- | --- | --- |
| application | 14be:001b | 1 (Cresnet io) | 0x02 OUT, 0x82 IN |
| bootloader | 14be:001a | 0 (the only one) | 0x01 OUT, 0x81 IN |

The bootloader shows the USB string 4 `CSIGN-BOOTLOADER [v...]`. In
bootloader mode the host sends and reads on interface 0.

## Packets

Download control packets have the type `04` and one sub-type byte:

| Packet | Bytes | Direction |
| --- | --- | --- |
| prepare for download | `02 02 04 04` | host to application, or host to bootloader |
| ready, block size | `xx 03 04 01 NN` | bootloader to host |
| end of block | `02 02 04 05` | host to bootloader |
| block accepted | `xx 03 04 01 NN` (type `04`, sub-type `01`) | bootloader to host |
| abort | `xx xx 04 06` | bootloader to host |
| end of update | `02 02 04 02` | host to bootloader |
| abort by the host | `02 02 04 06` | host to bootloader |
| prepare for a bootloader image | `02 02 04 01` | host to application, not used here |

The block size is `1 << NN` bytes. The vendor host starts with 512 and
replaces it with the value from the ready packet.

An S-record packet has the type `0D` and carries the record body: the
bytes after the count field of the record, that is the address, the data
and the checksum byte. The record type and the count are not sent.

| Byte | Value |
| --- | --- |
| 0 | `02` |
| 1 | count + 1 |
| 2 | `0D` |
| 3.. | address (4 bytes for S3), data, checksum |

Example, the tag record `S308BAD0ADD0444DE57A`:
`02 09 0D BA D0 AD D0 44 4D E5 7A`.

One packet is one USB packet of at most 64 bytes, so the record body
can have at most 61 bytes: 4 address bytes, 56 data bytes and the
checksum. `tsx-upg.py pack` writes 16 data bytes in each record.
`tsx-ledbar-flash` refuses a file with a longer record.

## Sequence

1. In application mode, send the prepare packet on interface 1. The
   application writes "UPG" into the bootloader mailbox and resets. The
   vendor host waits 5 s, then checks that the bar is in bootloader mode.
2. Wait 100 ms, then read on interface 0 until the ready packet comes.
   Keep its block size. Read until a read returns nothing. If no ready
   packet comes within 2 s, send the prepare packet on interface 0 one
   time, and read again (see "The ready packet" below).
3. Send the tag record (the first record of the file) as an S-record
   packet. Send end of block. Wait for the accept packet.
4. Send the other records in file order. Keep the address of the first
   record of the current block. Before a record whose address is above
   that address plus the block size, send end of block, wait for the
   accept packet, and start a new block with this record. A record at
   exactly that address stays in the current block. The vendor host
   uses the same rule.
5. After the last record, send end of update. Do not wait for an answer.
   The bootloader checks the image CRC and starts the application.
6. Wait for the application on USB (the vendor host polls for 10 s).

The vendor host polls the accept packet up to 200 times, with 1 ms
between empty reads (each read has a 1 s timeout in its driver). A
packet with sub-type `06` ends the update with an error. An S-record
with a bad checksum ends the upload with the end-of-update packet and
the "aborted" flag.

## The ready packet

The bootloader sends the ready packet one time, when it starts in update mode. Its USB write waits for the host to read the packet. If no host reads it in time, the bootloader drops the packet and sends nothing more. A bar that waits in bootloader mode for some time thus gives no ready packet to a new host.

The bootloader answers the prepare packet `02 02 04 04` on interface 0 with a new ready packet. The prepare packet also sets the update state back to the start: the bootloader waits for the tag record again. It erases nothing. The erase starts only at the end of the first block, after a good tag record.

`tsx-ledbar-flash` reads for 2 s first. A bootloader that started a short time before sends its ready packet in this time. If no ready packet comes, the tool sends the prepare packet one time and reads on.

A bootloader can also refuse the prepare packet: the USB write times out after 2 s. The tool then waits 1 s and sends the prepare packet again, three times in all. After the third timeout the tool stops. It sent no record, so the bootloader erased nothing. To recover, unplug the bar and plug it in again. The bootloader then starts the application when its CRC is good. Or run the tool again.

## Application side

The stock application acts on the prepare packet only. It does not answer it. A replacement application must do the same: write "UPG" into the mailbox at 0x200000F4 and reset. The bootloader then enumerates as 14be:001a and runs the sequence above.

## Troubleshooting

| Symptom | Cause | Fix |
| --- | --- | --- |
| No answer on USB after `IMGUPD` of the stock application | `IMGUPD` of the stock application writes "UPE". That selects a text monitor on a UART of the bar, which does not answer on USB | Do not use it for a USB update. Run `tsx-ledbar-flash port-reset`. This resets the USB bus of the port and does not cut the power of the bar |
| The load stops before the end | The bootloader keeps an image only when its CRC is good | Run the load again. The bar stays in the bootloader |
| `no ready packet from the bootloader` or `the bootloader takes no packets` | The bootloader sends no ready packet, also after the prepare packet, or it refuses the prepare packet (see "The ready packet") | Unplug the LED bar and plug it in again. Or run the load again |
