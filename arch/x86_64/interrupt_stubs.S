/*
 * 256 个独立 IDT vector stub，共享一个保存上下文/调用 C dispatcher 的入口。
 *
 * 无 CPU error-code 的向量先压 synthetic 0，再压 vector。
 * 有 CPU error-code 的 exception 只压 vector，因此统一布局始终是：
 *   vector, error_code, RIP, CS, RFLAGS, [RSP, SS]
 */

.text
.p2align 4

.extern idt_dispatch

.global idt_stub_0
.type idt_stub_0, @function
idt_stub_0:
    pushq $0
    pushq $0
    jmp idt_common_entry
.size idt_stub_0, .-idt_stub_0

.global idt_stub_1
.type idt_stub_1, @function
idt_stub_1:
    pushq $0
    pushq $1
    jmp idt_common_entry
.size idt_stub_1, .-idt_stub_1

.global idt_stub_2
.type idt_stub_2, @function
idt_stub_2:
    pushq $0
    pushq $2
    jmp idt_common_entry
.size idt_stub_2, .-idt_stub_2

.global idt_stub_3
.type idt_stub_3, @function
idt_stub_3:
    pushq $0
    pushq $3
    jmp idt_common_entry
.size idt_stub_3, .-idt_stub_3

.global idt_stub_4
.type idt_stub_4, @function
idt_stub_4:
    pushq $0
    pushq $4
    jmp idt_common_entry
.size idt_stub_4, .-idt_stub_4

.global idt_stub_5
.type idt_stub_5, @function
idt_stub_5:
    pushq $0
    pushq $5
    jmp idt_common_entry
.size idt_stub_5, .-idt_stub_5

.global idt_stub_6
.type idt_stub_6, @function
idt_stub_6:
    pushq $0
    pushq $6
    jmp idt_common_entry
.size idt_stub_6, .-idt_stub_6

.global idt_stub_7
.type idt_stub_7, @function
idt_stub_7:
    pushq $0
    pushq $7
    jmp idt_common_entry
.size idt_stub_7, .-idt_stub_7

.global idt_stub_8
.type idt_stub_8, @function
idt_stub_8:
    pushq $8
    jmp idt_common_entry
.size idt_stub_8, .-idt_stub_8

.global idt_stub_9
.type idt_stub_9, @function
idt_stub_9:
    pushq $0
    pushq $9
    jmp idt_common_entry
.size idt_stub_9, .-idt_stub_9

.global idt_stub_10
.type idt_stub_10, @function
idt_stub_10:
    pushq $10
    jmp idt_common_entry
.size idt_stub_10, .-idt_stub_10

.global idt_stub_11
.type idt_stub_11, @function
idt_stub_11:
    pushq $11
    jmp idt_common_entry
.size idt_stub_11, .-idt_stub_11

.global idt_stub_12
.type idt_stub_12, @function
idt_stub_12:
    pushq $12
    jmp idt_common_entry
.size idt_stub_12, .-idt_stub_12

.global idt_stub_13
.type idt_stub_13, @function
idt_stub_13:
    pushq $13
    jmp idt_common_entry
.size idt_stub_13, .-idt_stub_13

.global idt_stub_14
.type idt_stub_14, @function
idt_stub_14:
    pushq $14
    jmp idt_common_entry
.size idt_stub_14, .-idt_stub_14

.global idt_stub_15
.type idt_stub_15, @function
idt_stub_15:
    pushq $0
    pushq $15
    jmp idt_common_entry
.size idt_stub_15, .-idt_stub_15

.global idt_stub_16
.type idt_stub_16, @function
idt_stub_16:
    pushq $0
    pushq $16
    jmp idt_common_entry
.size idt_stub_16, .-idt_stub_16

.global idt_stub_17
.type idt_stub_17, @function
idt_stub_17:
    pushq $17
    jmp idt_common_entry
.size idt_stub_17, .-idt_stub_17

.global idt_stub_18
.type idt_stub_18, @function
idt_stub_18:
    pushq $0
    pushq $18
    jmp idt_common_entry
.size idt_stub_18, .-idt_stub_18

.global idt_stub_19
.type idt_stub_19, @function
idt_stub_19:
    pushq $0
    pushq $19
    jmp idt_common_entry
.size idt_stub_19, .-idt_stub_19

.global idt_stub_20
.type idt_stub_20, @function
idt_stub_20:
    pushq $0
    pushq $20
    jmp idt_common_entry
.size idt_stub_20, .-idt_stub_20

.global idt_stub_21
.type idt_stub_21, @function
idt_stub_21:
    pushq $21
    jmp idt_common_entry
.size idt_stub_21, .-idt_stub_21

.global idt_stub_22
.type idt_stub_22, @function
idt_stub_22:
    pushq $0
    pushq $22
    jmp idt_common_entry
.size idt_stub_22, .-idt_stub_22

.global idt_stub_23
.type idt_stub_23, @function
idt_stub_23:
    pushq $0
    pushq $23
    jmp idt_common_entry
.size idt_stub_23, .-idt_stub_23

.global idt_stub_24
.type idt_stub_24, @function
idt_stub_24:
    pushq $0
    pushq $24
    jmp idt_common_entry
.size idt_stub_24, .-idt_stub_24

.global idt_stub_25
.type idt_stub_25, @function
idt_stub_25:
    pushq $0
    pushq $25
    jmp idt_common_entry
.size idt_stub_25, .-idt_stub_25

.global idt_stub_26
.type idt_stub_26, @function
idt_stub_26:
    pushq $0
    pushq $26
    jmp idt_common_entry
.size idt_stub_26, .-idt_stub_26

.global idt_stub_27
.type idt_stub_27, @function
idt_stub_27:
    pushq $0
    pushq $27
    jmp idt_common_entry
.size idt_stub_27, .-idt_stub_27

.global idt_stub_28
.type idt_stub_28, @function
idt_stub_28:
    pushq $0
    pushq $28
    jmp idt_common_entry
.size idt_stub_28, .-idt_stub_28

.global idt_stub_29
.type idt_stub_29, @function
idt_stub_29:
    pushq $29
    jmp idt_common_entry
.size idt_stub_29, .-idt_stub_29

.global idt_stub_30
.type idt_stub_30, @function
idt_stub_30:
    pushq $30
    jmp idt_common_entry
.size idt_stub_30, .-idt_stub_30

.global idt_stub_31
.type idt_stub_31, @function
idt_stub_31:
    pushq $0
    pushq $31
    jmp idt_common_entry
.size idt_stub_31, .-idt_stub_31

.global idt_stub_32
.type idt_stub_32, @function
idt_stub_32:
    pushq $0
    pushq $32
    jmp idt_common_entry
.size idt_stub_32, .-idt_stub_32

.global idt_stub_33
.type idt_stub_33, @function
idt_stub_33:
    pushq $0
    pushq $33
    jmp idt_common_entry
.size idt_stub_33, .-idt_stub_33

.global idt_stub_34
.type idt_stub_34, @function
idt_stub_34:
    pushq $0
    pushq $34
    jmp idt_common_entry
.size idt_stub_34, .-idt_stub_34

.global idt_stub_35
.type idt_stub_35, @function
idt_stub_35:
    pushq $0
    pushq $35
    jmp idt_common_entry
.size idt_stub_35, .-idt_stub_35

.global idt_stub_36
.type idt_stub_36, @function
idt_stub_36:
    pushq $0
    pushq $36
    jmp idt_common_entry
.size idt_stub_36, .-idt_stub_36

.global idt_stub_37
.type idt_stub_37, @function
idt_stub_37:
    pushq $0
    pushq $37
    jmp idt_common_entry
.size idt_stub_37, .-idt_stub_37

.global idt_stub_38
.type idt_stub_38, @function
idt_stub_38:
    pushq $0
    pushq $38
    jmp idt_common_entry
.size idt_stub_38, .-idt_stub_38

.global idt_stub_39
.type idt_stub_39, @function
idt_stub_39:
    pushq $0
    pushq $39
    jmp idt_common_entry
.size idt_stub_39, .-idt_stub_39

.global idt_stub_40
.type idt_stub_40, @function
idt_stub_40:
    pushq $0
    pushq $40
    jmp idt_common_entry
.size idt_stub_40, .-idt_stub_40

.global idt_stub_41
.type idt_stub_41, @function
idt_stub_41:
    pushq $0
    pushq $41
    jmp idt_common_entry
.size idt_stub_41, .-idt_stub_41

.global idt_stub_42
.type idt_stub_42, @function
idt_stub_42:
    pushq $0
    pushq $42
    jmp idt_common_entry
.size idt_stub_42, .-idt_stub_42

.global idt_stub_43
.type idt_stub_43, @function
idt_stub_43:
    pushq $0
    pushq $43
    jmp idt_common_entry
.size idt_stub_43, .-idt_stub_43

.global idt_stub_44
.type idt_stub_44, @function
idt_stub_44:
    pushq $0
    pushq $44
    jmp idt_common_entry
.size idt_stub_44, .-idt_stub_44

.global idt_stub_45
.type idt_stub_45, @function
idt_stub_45:
    pushq $0
    pushq $45
    jmp idt_common_entry
.size idt_stub_45, .-idt_stub_45

.global idt_stub_46
.type idt_stub_46, @function
idt_stub_46:
    pushq $0
    pushq $46
    jmp idt_common_entry
.size idt_stub_46, .-idt_stub_46

.global idt_stub_47
.type idt_stub_47, @function
idt_stub_47:
    pushq $0
    pushq $47
    jmp idt_common_entry
.size idt_stub_47, .-idt_stub_47

.global idt_stub_48
.type idt_stub_48, @function
idt_stub_48:
    pushq $0
    pushq $48
    jmp idt_common_entry
.size idt_stub_48, .-idt_stub_48

.global idt_stub_49
.type idt_stub_49, @function
idt_stub_49:
    pushq $0
    pushq $49
    jmp idt_common_entry
.size idt_stub_49, .-idt_stub_49

.global idt_stub_50
.type idt_stub_50, @function
idt_stub_50:
    pushq $0
    pushq $50
    jmp idt_common_entry
.size idt_stub_50, .-idt_stub_50

.global idt_stub_51
.type idt_stub_51, @function
idt_stub_51:
    pushq $0
    pushq $51
    jmp idt_common_entry
.size idt_stub_51, .-idt_stub_51

.global idt_stub_52
.type idt_stub_52, @function
idt_stub_52:
    pushq $0
    pushq $52
    jmp idt_common_entry
.size idt_stub_52, .-idt_stub_52

.global idt_stub_53
.type idt_stub_53, @function
idt_stub_53:
    pushq $0
    pushq $53
    jmp idt_common_entry
.size idt_stub_53, .-idt_stub_53

.global idt_stub_54
.type idt_stub_54, @function
idt_stub_54:
    pushq $0
    pushq $54
    jmp idt_common_entry
.size idt_stub_54, .-idt_stub_54

.global idt_stub_55
.type idt_stub_55, @function
idt_stub_55:
    pushq $0
    pushq $55
    jmp idt_common_entry
.size idt_stub_55, .-idt_stub_55

.global idt_stub_56
.type idt_stub_56, @function
idt_stub_56:
    pushq $0
    pushq $56
    jmp idt_common_entry
.size idt_stub_56, .-idt_stub_56

.global idt_stub_57
.type idt_stub_57, @function
idt_stub_57:
    pushq $0
    pushq $57
    jmp idt_common_entry
.size idt_stub_57, .-idt_stub_57

.global idt_stub_58
.type idt_stub_58, @function
idt_stub_58:
    pushq $0
    pushq $58
    jmp idt_common_entry
.size idt_stub_58, .-idt_stub_58

.global idt_stub_59
.type idt_stub_59, @function
idt_stub_59:
    pushq $0
    pushq $59
    jmp idt_common_entry
.size idt_stub_59, .-idt_stub_59

.global idt_stub_60
.type idt_stub_60, @function
idt_stub_60:
    pushq $0
    pushq $60
    jmp idt_common_entry
.size idt_stub_60, .-idt_stub_60

.global idt_stub_61
.type idt_stub_61, @function
idt_stub_61:
    pushq $0
    pushq $61
    jmp idt_common_entry
.size idt_stub_61, .-idt_stub_61

.global idt_stub_62
.type idt_stub_62, @function
idt_stub_62:
    pushq $0
    pushq $62
    jmp idt_common_entry
.size idt_stub_62, .-idt_stub_62

.global idt_stub_63
.type idt_stub_63, @function
idt_stub_63:
    pushq $0
    pushq $63
    jmp idt_common_entry
.size idt_stub_63, .-idt_stub_63

.global idt_stub_64
.type idt_stub_64, @function
idt_stub_64:
    pushq $0
    pushq $64
    jmp idt_common_entry
.size idt_stub_64, .-idt_stub_64

.global idt_stub_65
.type idt_stub_65, @function
idt_stub_65:
    pushq $0
    pushq $65
    jmp idt_common_entry
.size idt_stub_65, .-idt_stub_65

.global idt_stub_66
.type idt_stub_66, @function
idt_stub_66:
    pushq $0
    pushq $66
    jmp idt_common_entry
.size idt_stub_66, .-idt_stub_66

.global idt_stub_67
.type idt_stub_67, @function
idt_stub_67:
    pushq $0
    pushq $67
    jmp idt_common_entry
.size idt_stub_67, .-idt_stub_67

.global idt_stub_68
.type idt_stub_68, @function
idt_stub_68:
    pushq $0
    pushq $68
    jmp idt_common_entry
.size idt_stub_68, .-idt_stub_68

.global idt_stub_69
.type idt_stub_69, @function
idt_stub_69:
    pushq $0
    pushq $69
    jmp idt_common_entry
.size idt_stub_69, .-idt_stub_69

.global idt_stub_70
.type idt_stub_70, @function
idt_stub_70:
    pushq $0
    pushq $70
    jmp idt_common_entry
.size idt_stub_70, .-idt_stub_70

.global idt_stub_71
.type idt_stub_71, @function
idt_stub_71:
    pushq $0
    pushq $71
    jmp idt_common_entry
.size idt_stub_71, .-idt_stub_71

.global idt_stub_72
.type idt_stub_72, @function
idt_stub_72:
    pushq $0
    pushq $72
    jmp idt_common_entry
.size idt_stub_72, .-idt_stub_72

.global idt_stub_73
.type idt_stub_73, @function
idt_stub_73:
    pushq $0
    pushq $73
    jmp idt_common_entry
.size idt_stub_73, .-idt_stub_73

.global idt_stub_74
.type idt_stub_74, @function
idt_stub_74:
    pushq $0
    pushq $74
    jmp idt_common_entry
.size idt_stub_74, .-idt_stub_74

.global idt_stub_75
.type idt_stub_75, @function
idt_stub_75:
    pushq $0
    pushq $75
    jmp idt_common_entry
.size idt_stub_75, .-idt_stub_75

.global idt_stub_76
.type idt_stub_76, @function
idt_stub_76:
    pushq $0
    pushq $76
    jmp idt_common_entry
.size idt_stub_76, .-idt_stub_76

.global idt_stub_77
.type idt_stub_77, @function
idt_stub_77:
    pushq $0
    pushq $77
    jmp idt_common_entry
.size idt_stub_77, .-idt_stub_77

.global idt_stub_78
.type idt_stub_78, @function
idt_stub_78:
    pushq $0
    pushq $78
    jmp idt_common_entry
.size idt_stub_78, .-idt_stub_78

.global idt_stub_79
.type idt_stub_79, @function
idt_stub_79:
    pushq $0
    pushq $79
    jmp idt_common_entry
.size idt_stub_79, .-idt_stub_79

.global idt_stub_80
.type idt_stub_80, @function
idt_stub_80:
    pushq $0
    pushq $80
    jmp idt_common_entry
.size idt_stub_80, .-idt_stub_80

.global idt_stub_81
.type idt_stub_81, @function
idt_stub_81:
    pushq $0
    pushq $81
    jmp idt_common_entry
.size idt_stub_81, .-idt_stub_81

.global idt_stub_82
.type idt_stub_82, @function
idt_stub_82:
    pushq $0
    pushq $82
    jmp idt_common_entry
.size idt_stub_82, .-idt_stub_82

.global idt_stub_83
.type idt_stub_83, @function
idt_stub_83:
    pushq $0
    pushq $83
    jmp idt_common_entry
.size idt_stub_83, .-idt_stub_83

.global idt_stub_84
.type idt_stub_84, @function
idt_stub_84:
    pushq $0
    pushq $84
    jmp idt_common_entry
.size idt_stub_84, .-idt_stub_84

.global idt_stub_85
.type idt_stub_85, @function
idt_stub_85:
    pushq $0
    pushq $85
    jmp idt_common_entry
.size idt_stub_85, .-idt_stub_85

.global idt_stub_86
.type idt_stub_86, @function
idt_stub_86:
    pushq $0
    pushq $86
    jmp idt_common_entry
.size idt_stub_86, .-idt_stub_86

.global idt_stub_87
.type idt_stub_87, @function
idt_stub_87:
    pushq $0
    pushq $87
    jmp idt_common_entry
.size idt_stub_87, .-idt_stub_87

.global idt_stub_88
.type idt_stub_88, @function
idt_stub_88:
    pushq $0
    pushq $88
    jmp idt_common_entry
.size idt_stub_88, .-idt_stub_88

.global idt_stub_89
.type idt_stub_89, @function
idt_stub_89:
    pushq $0
    pushq $89
    jmp idt_common_entry
.size idt_stub_89, .-idt_stub_89

.global idt_stub_90
.type idt_stub_90, @function
idt_stub_90:
    pushq $0
    pushq $90
    jmp idt_common_entry
.size idt_stub_90, .-idt_stub_90

.global idt_stub_91
.type idt_stub_91, @function
idt_stub_91:
    pushq $0
    pushq $91
    jmp idt_common_entry
.size idt_stub_91, .-idt_stub_91

.global idt_stub_92
.type idt_stub_92, @function
idt_stub_92:
    pushq $0
    pushq $92
    jmp idt_common_entry
.size idt_stub_92, .-idt_stub_92

.global idt_stub_93
.type idt_stub_93, @function
idt_stub_93:
    pushq $0
    pushq $93
    jmp idt_common_entry
.size idt_stub_93, .-idt_stub_93

.global idt_stub_94
.type idt_stub_94, @function
idt_stub_94:
    pushq $0
    pushq $94
    jmp idt_common_entry
.size idt_stub_94, .-idt_stub_94

.global idt_stub_95
.type idt_stub_95, @function
idt_stub_95:
    pushq $0
    pushq $95
    jmp idt_common_entry
.size idt_stub_95, .-idt_stub_95

.global idt_stub_96
.type idt_stub_96, @function
idt_stub_96:
    pushq $0
    pushq $96
    jmp idt_common_entry
.size idt_stub_96, .-idt_stub_96

.global idt_stub_97
.type idt_stub_97, @function
idt_stub_97:
    pushq $0
    pushq $97
    jmp idt_common_entry
.size idt_stub_97, .-idt_stub_97

.global idt_stub_98
.type idt_stub_98, @function
idt_stub_98:
    pushq $0
    pushq $98
    jmp idt_common_entry
.size idt_stub_98, .-idt_stub_98

.global idt_stub_99
.type idt_stub_99, @function
idt_stub_99:
    pushq $0
    pushq $99
    jmp idt_common_entry
.size idt_stub_99, .-idt_stub_99

.global idt_stub_100
.type idt_stub_100, @function
idt_stub_100:
    pushq $0
    pushq $100
    jmp idt_common_entry
.size idt_stub_100, .-idt_stub_100

.global idt_stub_101
.type idt_stub_101, @function
idt_stub_101:
    pushq $0
    pushq $101
    jmp idt_common_entry
.size idt_stub_101, .-idt_stub_101

.global idt_stub_102
.type idt_stub_102, @function
idt_stub_102:
    pushq $0
    pushq $102
    jmp idt_common_entry
.size idt_stub_102, .-idt_stub_102

.global idt_stub_103
.type idt_stub_103, @function
idt_stub_103:
    pushq $0
    pushq $103
    jmp idt_common_entry
.size idt_stub_103, .-idt_stub_103

.global idt_stub_104
.type idt_stub_104, @function
idt_stub_104:
    pushq $0
    pushq $104
    jmp idt_common_entry
.size idt_stub_104, .-idt_stub_104

.global idt_stub_105
.type idt_stub_105, @function
idt_stub_105:
    pushq $0
    pushq $105
    jmp idt_common_entry
.size idt_stub_105, .-idt_stub_105

.global idt_stub_106
.type idt_stub_106, @function
idt_stub_106:
    pushq $0
    pushq $106
    jmp idt_common_entry
.size idt_stub_106, .-idt_stub_106

.global idt_stub_107
.type idt_stub_107, @function
idt_stub_107:
    pushq $0
    pushq $107
    jmp idt_common_entry
.size idt_stub_107, .-idt_stub_107

.global idt_stub_108
.type idt_stub_108, @function
idt_stub_108:
    pushq $0
    pushq $108
    jmp idt_common_entry
.size idt_stub_108, .-idt_stub_108

.global idt_stub_109
.type idt_stub_109, @function
idt_stub_109:
    pushq $0
    pushq $109
    jmp idt_common_entry
.size idt_stub_109, .-idt_stub_109

.global idt_stub_110
.type idt_stub_110, @function
idt_stub_110:
    pushq $0
    pushq $110
    jmp idt_common_entry
.size idt_stub_110, .-idt_stub_110

.global idt_stub_111
.type idt_stub_111, @function
idt_stub_111:
    pushq $0
    pushq $111
    jmp idt_common_entry
.size idt_stub_111, .-idt_stub_111

.global idt_stub_112
.type idt_stub_112, @function
idt_stub_112:
    pushq $0
    pushq $112
    jmp idt_common_entry
.size idt_stub_112, .-idt_stub_112

.global idt_stub_113
.type idt_stub_113, @function
idt_stub_113:
    pushq $0
    pushq $113
    jmp idt_common_entry
.size idt_stub_113, .-idt_stub_113

.global idt_stub_114
.type idt_stub_114, @function
idt_stub_114:
    pushq $0
    pushq $114
    jmp idt_common_entry
.size idt_stub_114, .-idt_stub_114

.global idt_stub_115
.type idt_stub_115, @function
idt_stub_115:
    pushq $0
    pushq $115
    jmp idt_common_entry
.size idt_stub_115, .-idt_stub_115

.global idt_stub_116
.type idt_stub_116, @function
idt_stub_116:
    pushq $0
    pushq $116
    jmp idt_common_entry
.size idt_stub_116, .-idt_stub_116

.global idt_stub_117
.type idt_stub_117, @function
idt_stub_117:
    pushq $0
    pushq $117
    jmp idt_common_entry
.size idt_stub_117, .-idt_stub_117

.global idt_stub_118
.type idt_stub_118, @function
idt_stub_118:
    pushq $0
    pushq $118
    jmp idt_common_entry
.size idt_stub_118, .-idt_stub_118

.global idt_stub_119
.type idt_stub_119, @function
idt_stub_119:
    pushq $0
    pushq $119
    jmp idt_common_entry
.size idt_stub_119, .-idt_stub_119

.global idt_stub_120
.type idt_stub_120, @function
idt_stub_120:
    pushq $0
    pushq $120
    jmp idt_common_entry
.size idt_stub_120, .-idt_stub_120

.global idt_stub_121
.type idt_stub_121, @function
idt_stub_121:
    pushq $0
    pushq $121
    jmp idt_common_entry
.size idt_stub_121, .-idt_stub_121

.global idt_stub_122
.type idt_stub_122, @function
idt_stub_122:
    pushq $0
    pushq $122
    jmp idt_common_entry
.size idt_stub_122, .-idt_stub_122

.global idt_stub_123
.type idt_stub_123, @function
idt_stub_123:
    pushq $0
    pushq $123
    jmp idt_common_entry
.size idt_stub_123, .-idt_stub_123

.global idt_stub_124
.type idt_stub_124, @function
idt_stub_124:
    pushq $0
    pushq $124
    jmp idt_common_entry
.size idt_stub_124, .-idt_stub_124

.global idt_stub_125
.type idt_stub_125, @function
idt_stub_125:
    pushq $0
    pushq $125
    jmp idt_common_entry
.size idt_stub_125, .-idt_stub_125

.global idt_stub_126
.type idt_stub_126, @function
idt_stub_126:
    pushq $0
    pushq $126
    jmp idt_common_entry
.size idt_stub_126, .-idt_stub_126

.global idt_stub_127
.type idt_stub_127, @function
idt_stub_127:
    pushq $0
    pushq $127
    jmp idt_common_entry
.size idt_stub_127, .-idt_stub_127

.global idt_stub_128
.type idt_stub_128, @function
idt_stub_128:
    pushq $0
    pushq $128
    jmp idt_common_entry
.size idt_stub_128, .-idt_stub_128

.global idt_stub_129
.type idt_stub_129, @function
idt_stub_129:
    pushq $0
    pushq $129
    jmp idt_common_entry
.size idt_stub_129, .-idt_stub_129

.global idt_stub_130
.type idt_stub_130, @function
idt_stub_130:
    pushq $0
    pushq $130
    jmp idt_common_entry
.size idt_stub_130, .-idt_stub_130

.global idt_stub_131
.type idt_stub_131, @function
idt_stub_131:
    pushq $0
    pushq $131
    jmp idt_common_entry
.size idt_stub_131, .-idt_stub_131

.global idt_stub_132
.type idt_stub_132, @function
idt_stub_132:
    pushq $0
    pushq $132
    jmp idt_common_entry
.size idt_stub_132, .-idt_stub_132

.global idt_stub_133
.type idt_stub_133, @function
idt_stub_133:
    pushq $0
    pushq $133
    jmp idt_common_entry
.size idt_stub_133, .-idt_stub_133

.global idt_stub_134
.type idt_stub_134, @function
idt_stub_134:
    pushq $0
    pushq $134
    jmp idt_common_entry
.size idt_stub_134, .-idt_stub_134

.global idt_stub_135
.type idt_stub_135, @function
idt_stub_135:
    pushq $0
    pushq $135
    jmp idt_common_entry
.size idt_stub_135, .-idt_stub_135

.global idt_stub_136
.type idt_stub_136, @function
idt_stub_136:
    pushq $0
    pushq $136
    jmp idt_common_entry
.size idt_stub_136, .-idt_stub_136

.global idt_stub_137
.type idt_stub_137, @function
idt_stub_137:
    pushq $0
    pushq $137
    jmp idt_common_entry
.size idt_stub_137, .-idt_stub_137

.global idt_stub_138
.type idt_stub_138, @function
idt_stub_138:
    pushq $0
    pushq $138
    jmp idt_common_entry
.size idt_stub_138, .-idt_stub_138

.global idt_stub_139
.type idt_stub_139, @function
idt_stub_139:
    pushq $0
    pushq $139
    jmp idt_common_entry
.size idt_stub_139, .-idt_stub_139

.global idt_stub_140
.type idt_stub_140, @function
idt_stub_140:
    pushq $0
    pushq $140
    jmp idt_common_entry
.size idt_stub_140, .-idt_stub_140

.global idt_stub_141
.type idt_stub_141, @function
idt_stub_141:
    pushq $0
    pushq $141
    jmp idt_common_entry
.size idt_stub_141, .-idt_stub_141

.global idt_stub_142
.type idt_stub_142, @function
idt_stub_142:
    pushq $0
    pushq $142
    jmp idt_common_entry
.size idt_stub_142, .-idt_stub_142

.global idt_stub_143
.type idt_stub_143, @function
idt_stub_143:
    pushq $0
    pushq $143
    jmp idt_common_entry
.size idt_stub_143, .-idt_stub_143

.global idt_stub_144
.type idt_stub_144, @function
idt_stub_144:
    pushq $0
    pushq $144
    jmp idt_common_entry
.size idt_stub_144, .-idt_stub_144

.global idt_stub_145
.type idt_stub_145, @function
idt_stub_145:
    pushq $0
    pushq $145
    jmp idt_common_entry
.size idt_stub_145, .-idt_stub_145

.global idt_stub_146
.type idt_stub_146, @function
idt_stub_146:
    pushq $0
    pushq $146
    jmp idt_common_entry
.size idt_stub_146, .-idt_stub_146

.global idt_stub_147
.type idt_stub_147, @function
idt_stub_147:
    pushq $0
    pushq $147
    jmp idt_common_entry
.size idt_stub_147, .-idt_stub_147

.global idt_stub_148
.type idt_stub_148, @function
idt_stub_148:
    pushq $0
    pushq $148
    jmp idt_common_entry
.size idt_stub_148, .-idt_stub_148

.global idt_stub_149
.type idt_stub_149, @function
idt_stub_149:
    pushq $0
    pushq $149
    jmp idt_common_entry
.size idt_stub_149, .-idt_stub_149

.global idt_stub_150
.type idt_stub_150, @function
idt_stub_150:
    pushq $0
    pushq $150
    jmp idt_common_entry
.size idt_stub_150, .-idt_stub_150

.global idt_stub_151
.type idt_stub_151, @function
idt_stub_151:
    pushq $0
    pushq $151
    jmp idt_common_entry
.size idt_stub_151, .-idt_stub_151

.global idt_stub_152
.type idt_stub_152, @function
idt_stub_152:
    pushq $0
    pushq $152
    jmp idt_common_entry
.size idt_stub_152, .-idt_stub_152

.global idt_stub_153
.type idt_stub_153, @function
idt_stub_153:
    pushq $0
    pushq $153
    jmp idt_common_entry
.size idt_stub_153, .-idt_stub_153

.global idt_stub_154
.type idt_stub_154, @function
idt_stub_154:
    pushq $0
    pushq $154
    jmp idt_common_entry
.size idt_stub_154, .-idt_stub_154

.global idt_stub_155
.type idt_stub_155, @function
idt_stub_155:
    pushq $0
    pushq $155
    jmp idt_common_entry
.size idt_stub_155, .-idt_stub_155

.global idt_stub_156
.type idt_stub_156, @function
idt_stub_156:
    pushq $0
    pushq $156
    jmp idt_common_entry
.size idt_stub_156, .-idt_stub_156

.global idt_stub_157
.type idt_stub_157, @function
idt_stub_157:
    pushq $0
    pushq $157
    jmp idt_common_entry
.size idt_stub_157, .-idt_stub_157

.global idt_stub_158
.type idt_stub_158, @function
idt_stub_158:
    pushq $0
    pushq $158
    jmp idt_common_entry
.size idt_stub_158, .-idt_stub_158

.global idt_stub_159
.type idt_stub_159, @function
idt_stub_159:
    pushq $0
    pushq $159
    jmp idt_common_entry
.size idt_stub_159, .-idt_stub_159

.global idt_stub_160
.type idt_stub_160, @function
idt_stub_160:
    pushq $0
    pushq $160
    jmp idt_common_entry
.size idt_stub_160, .-idt_stub_160

.global idt_stub_161
.type idt_stub_161, @function
idt_stub_161:
    pushq $0
    pushq $161
    jmp idt_common_entry
.size idt_stub_161, .-idt_stub_161

.global idt_stub_162
.type idt_stub_162, @function
idt_stub_162:
    pushq $0
    pushq $162
    jmp idt_common_entry
.size idt_stub_162, .-idt_stub_162

.global idt_stub_163
.type idt_stub_163, @function
idt_stub_163:
    pushq $0
    pushq $163
    jmp idt_common_entry
.size idt_stub_163, .-idt_stub_163

.global idt_stub_164
.type idt_stub_164, @function
idt_stub_164:
    pushq $0
    pushq $164
    jmp idt_common_entry
.size idt_stub_164, .-idt_stub_164

.global idt_stub_165
.type idt_stub_165, @function
idt_stub_165:
    pushq $0
    pushq $165
    jmp idt_common_entry
.size idt_stub_165, .-idt_stub_165

.global idt_stub_166
.type idt_stub_166, @function
idt_stub_166:
    pushq $0
    pushq $166
    jmp idt_common_entry
.size idt_stub_166, .-idt_stub_166

.global idt_stub_167
.type idt_stub_167, @function
idt_stub_167:
    pushq $0
    pushq $167
    jmp idt_common_entry
.size idt_stub_167, .-idt_stub_167

.global idt_stub_168
.type idt_stub_168, @function
idt_stub_168:
    pushq $0
    pushq $168
    jmp idt_common_entry
.size idt_stub_168, .-idt_stub_168

.global idt_stub_169
.type idt_stub_169, @function
idt_stub_169:
    pushq $0
    pushq $169
    jmp idt_common_entry
.size idt_stub_169, .-idt_stub_169

.global idt_stub_170
.type idt_stub_170, @function
idt_stub_170:
    pushq $0
    pushq $170
    jmp idt_common_entry
.size idt_stub_170, .-idt_stub_170

.global idt_stub_171
.type idt_stub_171, @function
idt_stub_171:
    pushq $0
    pushq $171
    jmp idt_common_entry
.size idt_stub_171, .-idt_stub_171

.global idt_stub_172
.type idt_stub_172, @function
idt_stub_172:
    pushq $0
    pushq $172
    jmp idt_common_entry
.size idt_stub_172, .-idt_stub_172

.global idt_stub_173
.type idt_stub_173, @function
idt_stub_173:
    pushq $0
    pushq $173
    jmp idt_common_entry
.size idt_stub_173, .-idt_stub_173

.global idt_stub_174
.type idt_stub_174, @function
idt_stub_174:
    pushq $0
    pushq $174
    jmp idt_common_entry
.size idt_stub_174, .-idt_stub_174

.global idt_stub_175
.type idt_stub_175, @function
idt_stub_175:
    pushq $0
    pushq $175
    jmp idt_common_entry
.size idt_stub_175, .-idt_stub_175

.global idt_stub_176
.type idt_stub_176, @function
idt_stub_176:
    pushq $0
    pushq $176
    jmp idt_common_entry
.size idt_stub_176, .-idt_stub_176

.global idt_stub_177
.type idt_stub_177, @function
idt_stub_177:
    pushq $0
    pushq $177
    jmp idt_common_entry
.size idt_stub_177, .-idt_stub_177

.global idt_stub_178
.type idt_stub_178, @function
idt_stub_178:
    pushq $0
    pushq $178
    jmp idt_common_entry
.size idt_stub_178, .-idt_stub_178

.global idt_stub_179
.type idt_stub_179, @function
idt_stub_179:
    pushq $0
    pushq $179
    jmp idt_common_entry
.size idt_stub_179, .-idt_stub_179

.global idt_stub_180
.type idt_stub_180, @function
idt_stub_180:
    pushq $0
    pushq $180
    jmp idt_common_entry
.size idt_stub_180, .-idt_stub_180

.global idt_stub_181
.type idt_stub_181, @function
idt_stub_181:
    pushq $0
    pushq $181
    jmp idt_common_entry
.size idt_stub_181, .-idt_stub_181

.global idt_stub_182
.type idt_stub_182, @function
idt_stub_182:
    pushq $0
    pushq $182
    jmp idt_common_entry
.size idt_stub_182, .-idt_stub_182

.global idt_stub_183
.type idt_stub_183, @function
idt_stub_183:
    pushq $0
    pushq $183
    jmp idt_common_entry
.size idt_stub_183, .-idt_stub_183

.global idt_stub_184
.type idt_stub_184, @function
idt_stub_184:
    pushq $0
    pushq $184
    jmp idt_common_entry
.size idt_stub_184, .-idt_stub_184

.global idt_stub_185
.type idt_stub_185, @function
idt_stub_185:
    pushq $0
    pushq $185
    jmp idt_common_entry
.size idt_stub_185, .-idt_stub_185

.global idt_stub_186
.type idt_stub_186, @function
idt_stub_186:
    pushq $0
    pushq $186
    jmp idt_common_entry
.size idt_stub_186, .-idt_stub_186

.global idt_stub_187
.type idt_stub_187, @function
idt_stub_187:
    pushq $0
    pushq $187
    jmp idt_common_entry
.size idt_stub_187, .-idt_stub_187

.global idt_stub_188
.type idt_stub_188, @function
idt_stub_188:
    pushq $0
    pushq $188
    jmp idt_common_entry
.size idt_stub_188, .-idt_stub_188

.global idt_stub_189
.type idt_stub_189, @function
idt_stub_189:
    pushq $0
    pushq $189
    jmp idt_common_entry
.size idt_stub_189, .-idt_stub_189

.global idt_stub_190
.type idt_stub_190, @function
idt_stub_190:
    pushq $0
    pushq $190
    jmp idt_common_entry
.size idt_stub_190, .-idt_stub_190

.global idt_stub_191
.type idt_stub_191, @function
idt_stub_191:
    pushq $0
    pushq $191
    jmp idt_common_entry
.size idt_stub_191, .-idt_stub_191

.global idt_stub_192
.type idt_stub_192, @function
idt_stub_192:
    pushq $0
    pushq $192
    jmp idt_common_entry
.size idt_stub_192, .-idt_stub_192

.global idt_stub_193
.type idt_stub_193, @function
idt_stub_193:
    pushq $0
    pushq $193
    jmp idt_common_entry
.size idt_stub_193, .-idt_stub_193

.global idt_stub_194
.type idt_stub_194, @function
idt_stub_194:
    pushq $0
    pushq $194
    jmp idt_common_entry
.size idt_stub_194, .-idt_stub_194

.global idt_stub_195
.type idt_stub_195, @function
idt_stub_195:
    pushq $0
    pushq $195
    jmp idt_common_entry
.size idt_stub_195, .-idt_stub_195

.global idt_stub_196
.type idt_stub_196, @function
idt_stub_196:
    pushq $0
    pushq $196
    jmp idt_common_entry
.size idt_stub_196, .-idt_stub_196

.global idt_stub_197
.type idt_stub_197, @function
idt_stub_197:
    pushq $0
    pushq $197
    jmp idt_common_entry
.size idt_stub_197, .-idt_stub_197

.global idt_stub_198
.type idt_stub_198, @function
idt_stub_198:
    pushq $0
    pushq $198
    jmp idt_common_entry
.size idt_stub_198, .-idt_stub_198

.global idt_stub_199
.type idt_stub_199, @function
idt_stub_199:
    pushq $0
    pushq $199
    jmp idt_common_entry
.size idt_stub_199, .-idt_stub_199

.global idt_stub_200
.type idt_stub_200, @function
idt_stub_200:
    pushq $0
    pushq $200
    jmp idt_common_entry
.size idt_stub_200, .-idt_stub_200

.global idt_stub_201
.type idt_stub_201, @function
idt_stub_201:
    pushq $0
    pushq $201
    jmp idt_common_entry
.size idt_stub_201, .-idt_stub_201

.global idt_stub_202
.type idt_stub_202, @function
idt_stub_202:
    pushq $0
    pushq $202
    jmp idt_common_entry
.size idt_stub_202, .-idt_stub_202

.global idt_stub_203
.type idt_stub_203, @function
idt_stub_203:
    pushq $0
    pushq $203
    jmp idt_common_entry
.size idt_stub_203, .-idt_stub_203

.global idt_stub_204
.type idt_stub_204, @function
idt_stub_204:
    pushq $0
    pushq $204
    jmp idt_common_entry
.size idt_stub_204, .-idt_stub_204

.global idt_stub_205
.type idt_stub_205, @function
idt_stub_205:
    pushq $0
    pushq $205
    jmp idt_common_entry
.size idt_stub_205, .-idt_stub_205

.global idt_stub_206
.type idt_stub_206, @function
idt_stub_206:
    pushq $0
    pushq $206
    jmp idt_common_entry
.size idt_stub_206, .-idt_stub_206

.global idt_stub_207
.type idt_stub_207, @function
idt_stub_207:
    pushq $0
    pushq $207
    jmp idt_common_entry
.size idt_stub_207, .-idt_stub_207

.global idt_stub_208
.type idt_stub_208, @function
idt_stub_208:
    pushq $0
    pushq $208
    jmp idt_common_entry
.size idt_stub_208, .-idt_stub_208

.global idt_stub_209
.type idt_stub_209, @function
idt_stub_209:
    pushq $0
    pushq $209
    jmp idt_common_entry
.size idt_stub_209, .-idt_stub_209

.global idt_stub_210
.type idt_stub_210, @function
idt_stub_210:
    pushq $0
    pushq $210
    jmp idt_common_entry
.size idt_stub_210, .-idt_stub_210

.global idt_stub_211
.type idt_stub_211, @function
idt_stub_211:
    pushq $0
    pushq $211
    jmp idt_common_entry
.size idt_stub_211, .-idt_stub_211

.global idt_stub_212
.type idt_stub_212, @function
idt_stub_212:
    pushq $0
    pushq $212
    jmp idt_common_entry
.size idt_stub_212, .-idt_stub_212

.global idt_stub_213
.type idt_stub_213, @function
idt_stub_213:
    pushq $0
    pushq $213
    jmp idt_common_entry
.size idt_stub_213, .-idt_stub_213

.global idt_stub_214
.type idt_stub_214, @function
idt_stub_214:
    pushq $0
    pushq $214
    jmp idt_common_entry
.size idt_stub_214, .-idt_stub_214

.global idt_stub_215
.type idt_stub_215, @function
idt_stub_215:
    pushq $0
    pushq $215
    jmp idt_common_entry
.size idt_stub_215, .-idt_stub_215

.global idt_stub_216
.type idt_stub_216, @function
idt_stub_216:
    pushq $0
    pushq $216
    jmp idt_common_entry
.size idt_stub_216, .-idt_stub_216

.global idt_stub_217
.type idt_stub_217, @function
idt_stub_217:
    pushq $0
    pushq $217
    jmp idt_common_entry
.size idt_stub_217, .-idt_stub_217

.global idt_stub_218
.type idt_stub_218, @function
idt_stub_218:
    pushq $0
    pushq $218
    jmp idt_common_entry
.size idt_stub_218, .-idt_stub_218

.global idt_stub_219
.type idt_stub_219, @function
idt_stub_219:
    pushq $0
    pushq $219
    jmp idt_common_entry
.size idt_stub_219, .-idt_stub_219

.global idt_stub_220
.type idt_stub_220, @function
idt_stub_220:
    pushq $0
    pushq $220
    jmp idt_common_entry
.size idt_stub_220, .-idt_stub_220

.global idt_stub_221
.type idt_stub_221, @function
idt_stub_221:
    pushq $0
    pushq $221
    jmp idt_common_entry
.size idt_stub_221, .-idt_stub_221

.global idt_stub_222
.type idt_stub_222, @function
idt_stub_222:
    pushq $0
    pushq $222
    jmp idt_common_entry
.size idt_stub_222, .-idt_stub_222

.global idt_stub_223
.type idt_stub_223, @function
idt_stub_223:
    pushq $0
    pushq $223
    jmp idt_common_entry
.size idt_stub_223, .-idt_stub_223

.global idt_stub_224
.type idt_stub_224, @function
idt_stub_224:
    pushq $0
    pushq $224
    jmp idt_common_entry
.size idt_stub_224, .-idt_stub_224

.global idt_stub_225
.type idt_stub_225, @function
idt_stub_225:
    pushq $0
    pushq $225
    jmp idt_common_entry
.size idt_stub_225, .-idt_stub_225

.global idt_stub_226
.type idt_stub_226, @function
idt_stub_226:
    pushq $0
    pushq $226
    jmp idt_common_entry
.size idt_stub_226, .-idt_stub_226

.global idt_stub_227
.type idt_stub_227, @function
idt_stub_227:
    pushq $0
    pushq $227
    jmp idt_common_entry
.size idt_stub_227, .-idt_stub_227

.global idt_stub_228
.type idt_stub_228, @function
idt_stub_228:
    pushq $0
    pushq $228
    jmp idt_common_entry
.size idt_stub_228, .-idt_stub_228

.global idt_stub_229
.type idt_stub_229, @function
idt_stub_229:
    pushq $0
    pushq $229
    jmp idt_common_entry
.size idt_stub_229, .-idt_stub_229

.global idt_stub_230
.type idt_stub_230, @function
idt_stub_230:
    pushq $0
    pushq $230
    jmp idt_common_entry
.size idt_stub_230, .-idt_stub_230

.global idt_stub_231
.type idt_stub_231, @function
idt_stub_231:
    pushq $0
    pushq $231
    jmp idt_common_entry
.size idt_stub_231, .-idt_stub_231

.global idt_stub_232
.type idt_stub_232, @function
idt_stub_232:
    pushq $0
    pushq $232
    jmp idt_common_entry
.size idt_stub_232, .-idt_stub_232

.global idt_stub_233
.type idt_stub_233, @function
idt_stub_233:
    pushq $0
    pushq $233
    jmp idt_common_entry
.size idt_stub_233, .-idt_stub_233

.global idt_stub_234
.type idt_stub_234, @function
idt_stub_234:
    pushq $0
    pushq $234
    jmp idt_common_entry
.size idt_stub_234, .-idt_stub_234

.global idt_stub_235
.type idt_stub_235, @function
idt_stub_235:
    pushq $0
    pushq $235
    jmp idt_common_entry
.size idt_stub_235, .-idt_stub_235

.global idt_stub_236
.type idt_stub_236, @function
idt_stub_236:
    pushq $0
    pushq $236
    jmp idt_common_entry
.size idt_stub_236, .-idt_stub_236

.global idt_stub_237
.type idt_stub_237, @function
idt_stub_237:
    pushq $0
    pushq $237
    jmp idt_common_entry
.size idt_stub_237, .-idt_stub_237

.global idt_stub_238
.type idt_stub_238, @function
idt_stub_238:
    pushq $0
    pushq $238
    jmp idt_common_entry
.size idt_stub_238, .-idt_stub_238

.global idt_stub_239
.type idt_stub_239, @function
idt_stub_239:
    pushq $0
    pushq $239
    jmp idt_common_entry
.size idt_stub_239, .-idt_stub_239

.global idt_stub_240
.type idt_stub_240, @function
idt_stub_240:
    pushq $0
    pushq $240
    jmp idt_common_entry
.size idt_stub_240, .-idt_stub_240

.global idt_stub_241
.type idt_stub_241, @function
idt_stub_241:
    pushq $0
    pushq $241
    jmp idt_common_entry
.size idt_stub_241, .-idt_stub_241

.global idt_stub_242
.type idt_stub_242, @function
idt_stub_242:
    pushq $0
    pushq $242
    jmp idt_common_entry
.size idt_stub_242, .-idt_stub_242

.global idt_stub_243
.type idt_stub_243, @function
idt_stub_243:
    pushq $0
    pushq $243
    jmp idt_common_entry
.size idt_stub_243, .-idt_stub_243

.global idt_stub_244
.type idt_stub_244, @function
idt_stub_244:
    pushq $0
    pushq $244
    jmp idt_common_entry
.size idt_stub_244, .-idt_stub_244

.global idt_stub_245
.type idt_stub_245, @function
idt_stub_245:
    pushq $0
    pushq $245
    jmp idt_common_entry
.size idt_stub_245, .-idt_stub_245

.global idt_stub_246
.type idt_stub_246, @function
idt_stub_246:
    pushq $0
    pushq $246
    jmp idt_common_entry
.size idt_stub_246, .-idt_stub_246

.global idt_stub_247
.type idt_stub_247, @function
idt_stub_247:
    pushq $0
    pushq $247
    jmp idt_common_entry
.size idt_stub_247, .-idt_stub_247

.global idt_stub_248
.type idt_stub_248, @function
idt_stub_248:
    pushq $0
    pushq $248
    jmp idt_common_entry
.size idt_stub_248, .-idt_stub_248

.global idt_stub_249
.type idt_stub_249, @function
idt_stub_249:
    pushq $0
    pushq $249
    jmp idt_common_entry
.size idt_stub_249, .-idt_stub_249

.global idt_stub_250
.type idt_stub_250, @function
idt_stub_250:
    pushq $0
    pushq $250
    jmp idt_common_entry
.size idt_stub_250, .-idt_stub_250

.global idt_stub_251
.type idt_stub_251, @function
idt_stub_251:
    pushq $0
    pushq $251
    jmp idt_common_entry
.size idt_stub_251, .-idt_stub_251

.global idt_stub_252
.type idt_stub_252, @function
idt_stub_252:
    pushq $0
    pushq $252
    jmp idt_common_entry
.size idt_stub_252, .-idt_stub_252

.global idt_stub_253
.type idt_stub_253, @function
idt_stub_253:
    pushq $0
    pushq $253
    jmp idt_common_entry
.size idt_stub_253, .-idt_stub_253

.global idt_stub_254
.type idt_stub_254, @function
idt_stub_254:
    pushq $0
    pushq $254
    jmp idt_common_entry
.size idt_stub_254, .-idt_stub_254

.global idt_stub_255
.type idt_stub_255, @function
idt_stub_255:
    pushq $0
    pushq $255
    jmp idt_common_entry
.size idt_stub_255, .-idt_stub_255

.global idt_common_entry
.type idt_common_entry, @function
idt_common_entry:
    cld

    pushq %r15
    pushq %r14
    pushq %r13
    pushq %r12
    pushq %r11
    pushq %r10
    pushq %r9
    pushq %r8
    pushq %rdi
    pushq %rsi
    pushq %rbp
    pushq %rbx
    pushq %rdx
    pushq %rcx
    pushq %rax

    /*
     * C ABI 要求 call 前 RSP 16-byte aligned；硬件 interrupt frame 的
     * 原始对齐不能假定，所以保存 frame pointer 后临时向下对齐。
     * RBX 已经保存在 frame 中，而且 SysV callee 必须保持 RBX。
     */
    movq %rsp, %rbx
    movq %rsp, %rdi
    andq $-16, %rsp
    call idt_dispatch
    movq %rbx, %rsp

    popq %rax
    popq %rcx
    popq %rdx
    popq %rbx
    popq %rbp
    popq %rsi
    popq %rdi
    popq %r8
    popq %r9
    popq %r10
    popq %r11
    popq %r12
    popq %r13
    popq %r14
    popq %r15
    addq $16, %rsp
    iretq
.size idt_common_entry, .-idt_common_entry

.section .rodata
.p2align 3
.global idt_stub_table
.type idt_stub_table, @object
idt_stub_table:
    .quad idt_stub_0
    .quad idt_stub_1
    .quad idt_stub_2
    .quad idt_stub_3
    .quad idt_stub_4
    .quad idt_stub_5
    .quad idt_stub_6
    .quad idt_stub_7
    .quad idt_stub_8
    .quad idt_stub_9
    .quad idt_stub_10
    .quad idt_stub_11
    .quad idt_stub_12
    .quad idt_stub_13
    .quad idt_stub_14
    .quad idt_stub_15
    .quad idt_stub_16
    .quad idt_stub_17
    .quad idt_stub_18
    .quad idt_stub_19
    .quad idt_stub_20
    .quad idt_stub_21
    .quad idt_stub_22
    .quad idt_stub_23
    .quad idt_stub_24
    .quad idt_stub_25
    .quad idt_stub_26
    .quad idt_stub_27
    .quad idt_stub_28
    .quad idt_stub_29
    .quad idt_stub_30
    .quad idt_stub_31
    .quad idt_stub_32
    .quad idt_stub_33
    .quad idt_stub_34
    .quad idt_stub_35
    .quad idt_stub_36
    .quad idt_stub_37
    .quad idt_stub_38
    .quad idt_stub_39
    .quad idt_stub_40
    .quad idt_stub_41
    .quad idt_stub_42
    .quad idt_stub_43
    .quad idt_stub_44
    .quad idt_stub_45
    .quad idt_stub_46
    .quad idt_stub_47
    .quad idt_stub_48
    .quad idt_stub_49
    .quad idt_stub_50
    .quad idt_stub_51
    .quad idt_stub_52
    .quad idt_stub_53
    .quad idt_stub_54
    .quad idt_stub_55
    .quad idt_stub_56
    .quad idt_stub_57
    .quad idt_stub_58
    .quad idt_stub_59
    .quad idt_stub_60
    .quad idt_stub_61
    .quad idt_stub_62
    .quad idt_stub_63
    .quad idt_stub_64
    .quad idt_stub_65
    .quad idt_stub_66
    .quad idt_stub_67
    .quad idt_stub_68
    .quad idt_stub_69
    .quad idt_stub_70
    .quad idt_stub_71
    .quad idt_stub_72
    .quad idt_stub_73
    .quad idt_stub_74
    .quad idt_stub_75
    .quad idt_stub_76
    .quad idt_stub_77
    .quad idt_stub_78
    .quad idt_stub_79
    .quad idt_stub_80
    .quad idt_stub_81
    .quad idt_stub_82
    .quad idt_stub_83
    .quad idt_stub_84
    .quad idt_stub_85
    .quad idt_stub_86
    .quad idt_stub_87
    .quad idt_stub_88
    .quad idt_stub_89
    .quad idt_stub_90
    .quad idt_stub_91
    .quad idt_stub_92
    .quad idt_stub_93
    .quad idt_stub_94
    .quad idt_stub_95
    .quad idt_stub_96
    .quad idt_stub_97
    .quad idt_stub_98
    .quad idt_stub_99
    .quad idt_stub_100
    .quad idt_stub_101
    .quad idt_stub_102
    .quad idt_stub_103
    .quad idt_stub_104
    .quad idt_stub_105
    .quad idt_stub_106
    .quad idt_stub_107
    .quad idt_stub_108
    .quad idt_stub_109
    .quad idt_stub_110
    .quad idt_stub_111
    .quad idt_stub_112
    .quad idt_stub_113
    .quad idt_stub_114
    .quad idt_stub_115
    .quad idt_stub_116
    .quad idt_stub_117
    .quad idt_stub_118
    .quad idt_stub_119
    .quad idt_stub_120
    .quad idt_stub_121
    .quad idt_stub_122
    .quad idt_stub_123
    .quad idt_stub_124
    .quad idt_stub_125
    .quad idt_stub_126
    .quad idt_stub_127
    .quad idt_stub_128
    .quad idt_stub_129
    .quad idt_stub_130
    .quad idt_stub_131
    .quad idt_stub_132
    .quad idt_stub_133
    .quad idt_stub_134
    .quad idt_stub_135
    .quad idt_stub_136
    .quad idt_stub_137
    .quad idt_stub_138
    .quad idt_stub_139
    .quad idt_stub_140
    .quad idt_stub_141
    .quad idt_stub_142
    .quad idt_stub_143
    .quad idt_stub_144
    .quad idt_stub_145
    .quad idt_stub_146
    .quad idt_stub_147
    .quad idt_stub_148
    .quad idt_stub_149
    .quad idt_stub_150
    .quad idt_stub_151
    .quad idt_stub_152
    .quad idt_stub_153
    .quad idt_stub_154
    .quad idt_stub_155
    .quad idt_stub_156
    .quad idt_stub_157
    .quad idt_stub_158
    .quad idt_stub_159
    .quad idt_stub_160
    .quad idt_stub_161
    .quad idt_stub_162
    .quad idt_stub_163
    .quad idt_stub_164
    .quad idt_stub_165
    .quad idt_stub_166
    .quad idt_stub_167
    .quad idt_stub_168
    .quad idt_stub_169
    .quad idt_stub_170
    .quad idt_stub_171
    .quad idt_stub_172
    .quad idt_stub_173
    .quad idt_stub_174
    .quad idt_stub_175
    .quad idt_stub_176
    .quad idt_stub_177
    .quad idt_stub_178
    .quad idt_stub_179
    .quad idt_stub_180
    .quad idt_stub_181
    .quad idt_stub_182
    .quad idt_stub_183
    .quad idt_stub_184
    .quad idt_stub_185
    .quad idt_stub_186
    .quad idt_stub_187
    .quad idt_stub_188
    .quad idt_stub_189
    .quad idt_stub_190
    .quad idt_stub_191
    .quad idt_stub_192
    .quad idt_stub_193
    .quad idt_stub_194
    .quad idt_stub_195
    .quad idt_stub_196
    .quad idt_stub_197
    .quad idt_stub_198
    .quad idt_stub_199
    .quad idt_stub_200
    .quad idt_stub_201
    .quad idt_stub_202
    .quad idt_stub_203
    .quad idt_stub_204
    .quad idt_stub_205
    .quad idt_stub_206
    .quad idt_stub_207
    .quad idt_stub_208
    .quad idt_stub_209
    .quad idt_stub_210
    .quad idt_stub_211
    .quad idt_stub_212
    .quad idt_stub_213
    .quad idt_stub_214
    .quad idt_stub_215
    .quad idt_stub_216
    .quad idt_stub_217
    .quad idt_stub_218
    .quad idt_stub_219
    .quad idt_stub_220
    .quad idt_stub_221
    .quad idt_stub_222
    .quad idt_stub_223
    .quad idt_stub_224
    .quad idt_stub_225
    .quad idt_stub_226
    .quad idt_stub_227
    .quad idt_stub_228
    .quad idt_stub_229
    .quad idt_stub_230
    .quad idt_stub_231
    .quad idt_stub_232
    .quad idt_stub_233
    .quad idt_stub_234
    .quad idt_stub_235
    .quad idt_stub_236
    .quad idt_stub_237
    .quad idt_stub_238
    .quad idt_stub_239
    .quad idt_stub_240
    .quad idt_stub_241
    .quad idt_stub_242
    .quad idt_stub_243
    .quad idt_stub_244
    .quad idt_stub_245
    .quad idt_stub_246
    .quad idt_stub_247
    .quad idt_stub_248
    .quad idt_stub_249
    .quad idt_stub_250
    .quad idt_stub_251
    .quad idt_stub_252
    .quad idt_stub_253
    .quad idt_stub_254
    .quad idt_stub_255
.size idt_stub_table, .-idt_stub_table
