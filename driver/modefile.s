; DEVS:AudioModes/CA0106 - AHI mode descriptor for ca0106.audio
; Assemble with: vasmm68k_mot -Fbin -o CA0106 modefile.s
;
; Audio ID $00CA00xx is provisional (not allocated by the AHI maintainer).

TAG_DONE        equ     0
AHI_TagBase     equ     $80000000
AHI_TagBaseR    equ     $80008000
AHIDB_AudioID   equ     AHI_TagBase+100
AHIDB_Volume    equ     AHI_TagBase+103
AHIDB_Panning   equ     AHI_TagBase+104
AHIDB_Stereo    equ     AHI_TagBase+105
AHIDB_HiFi      equ     AHI_TagBase+106
AHIDB_MultTable equ     AHI_TagBase+108
AHIDB_Name      equ     AHI_TagBaseR+109

TRUE            equ     1
FALSE           equ     0

mode    macro   ; id, panning, hifi, name
        dc.b    "AUDM"
        dc.l    .e\@-.s\@
.s\@:
        dc.l    AHIDB_AudioID,\1
        dc.l    AHIDB_Volume,TRUE
        dc.l    AHIDB_Panning,\2
        dc.l    AHIDB_Stereo,TRUE
        dc.l    AHIDB_HiFi,\3
        dc.l    AHIDB_MultTable,FALSE
        dc.l    AHIDB_Name,.n\@-.s\@
        dc.l    TAG_DONE
.n\@:   dc.b    \4,0
        even
.e\@:
        endm

        dc.b    "FORM"
        dc.l    formend-start
start:
        dc.b    "AHIM"

        dc.b    "AUDN"
        dc.l    .e-.s
.s:     dc.b    "ca0106",0
        even
.e:

        mode    $00CA0001,TRUE,FALSE,"CA0106: 16 bit stereo++"
        mode    $00CA0002,FALSE,FALSE,"CA0106: 16 bit stereo"
        mode    $00CA0003,TRUE,TRUE,"CA0106: HiFi 24 bit stereo++"
        mode    $00CA0004,FALSE,TRUE,"CA0106: HiFi 24 bit stereo"
formend:
