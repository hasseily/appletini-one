#include "net_json.h"
#include <string.h>

enum { L_IDLE, L_STRING, L_NUMBER, L_TRUE, L_FALSE, L_NULL };
enum { O_KEY_END, O_KEY, O_COLON, O_VALUE, O_COMMA, A_VALUE_END, A_VALUE, A_COMMA };
static int white(uint8_t c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }
static int hex(uint8_t c)
{ return c >= '0' && c <= '9' ? c-'0' : c >= 'a' && c <= 'f' ? c-'a'+10 : c >= 'A' && c <= 'F' ? c-'A'+10 : -1; }
static void value_done(net_json_t *j)
{
    if (!j->depth) j->root = 1;
    else {
        net_json_frame_t *f = &j->stack[j->depth-1U];
        f->expect = j->tokens[f->token].type == NJ_OBJECT ? O_COMMA : A_COMMA;
    }
}
static int token(net_json_t *j, unsigned type)
{
    if (j->count == NET_JSON_TOKENS) return -1;
    j->current = j->count++;
    j->tokens[j->current].start = j->at;
    j->tokens[j->current].end = 0;
    j->tokens[j->current].next = j->count;
    j->tokens[j->current].type = (uint8_t)type;
    return 0;
}
static int number_byte(net_json_t *j, uint8_t c)
{
    /* 0 sign/start, 1 zero, 2 integer, 3 dot, 4 fraction,
     * 5 exponent, 6 exponent sign, 7 exponent digits. */
    switch (j->number) {
    case 0: if (c == '0') j->number=1; else if(c>='1'&&c<='9')j->number=2; else return -1; break;
    case 1: case 2:
        if(c>='0'&&c<='9') { if(j->number==1)return -1; }
        else if(c=='.')j->number=3;
        else if(c=='e'||c=='E')j->number=5;
        else return 1;
        break;
    case 3: if(c>='0'&&c<='9')j->number=4; else return -1; break;
    case 4:
        if(c=='e'||c=='E')j->number=5;
        else if(c<'0'||c>'9')return 1;
        break;
    case 5:
        if(c=='+'||c=='-')j->number=6;
        else if(c>='0'&&c<='9')j->number=7; else return -1;
        break;
    case 6: if(c>='0'&&c<='9')j->number=7;else return -1;break;
    case 7: if(c<'0'||c>'9')return 1;break;
    }
    return 0;
}
static int string_byte(net_json_t *j, uint8_t c)
{
    if (j->utf8_left) {
        if ((c & 0xC0U) != 0x80U) return -1;
        j->utf8_code = (j->utf8_code << 6) | (c & 63U);
        if (!--j->utf8_left && (j->utf8_code < j->utf8_min || j->utf8_code > 0x10FFFFU ||
            (j->utf8_code >= 0xD800U && j->utf8_code <= 0xDFFFU))) return -1;
        return 0;
    }
    if (j->digits) {
        int h = hex(c);
        if(h < 0)return -1;
        j->unicode=(j->unicode<<4)|(unsigned)h;
        if (!--j->digits) {
            if(j->high) {
                if(j->unicode<0xDC00U||j->unicode>0xDFFFU)return -1;
                j->high=0;
            } else if(j->unicode>=0xD800U&&j->unicode<=0xDBFFU) {j->high=(uint16_t)j->unicode;j->escape=2;}
            else if(j->unicode>=0xDC00U&&j->unicode<=0xDFFFU)return -1;
        }
        return 0;
    }
    if(j->escape==2) {if(c!='\\')return -1;j->escape=3;return 0;}
    if(j->escape) {
        if(j->escape==3&&c!='u')return -1;
        j->escape=0;
        if(c=='u'){j->digits=4;j->unicode=0;return 0;}
        return c && strchr("\"\\/bfnrt",c) ? 0 : -1;
    }
    if(c=='\\'){j->escape=1;return 0;}
    if(c=='"')return 1;
    if(c<32)return -1;
    if(c<128)return 0;
    if(c>=0xC2&&c<=0xDF){j->utf8_left=1;j->utf8_code=c&31U;j->utf8_min=0x80;}
    else if(c>=0xE0&&c<=0xEF){j->utf8_left=2;j->utf8_code=c&15U;j->utf8_min=0x800;}
    else if(c>=0xF0&&c<=0xF4){j->utf8_left=3;j->utf8_code=c&7U;j->utf8_min=0x10000;}
    else return -1;
    return 0;
}
static int consume(net_json_t *j, uint8_t c)
{
    if(j->lex==L_STRING) {
        int r=string_byte(j,c);
        if(r<0)return -1;
        if(r) {
            net_json_token_t *t=&j->tokens[j->current];
            t->end=j->at+1U;j->lex=L_IDLE;
            if(t->type & 0x80U)j->stack[j->depth-1U].expect=O_COLON;
            else value_done(j);
        }
        return 0;
    }
    if(j->lex==L_NUMBER) {
        int r=number_byte(j,c);
        if(r<0)return -1;
        if(!r)return 0;
        j->tokens[j->current].end=j->at;j->lex=L_IDLE;value_done(j);
        /* This delimiter still belongs to the grammar. */
    } else if(j->lex>=L_TRUE) {
        const char *s=j->lex==L_TRUE?"true":j->lex==L_FALSE?"false":"null";
        if(c!=(uint8_t)s[j->literal_at++])return -1;
        if(!s[j->literal_at]) {j->tokens[j->current].end=j->at+1U;j->lex=L_IDLE;value_done(j);}
        return 0;
    }
    if(white(c))return 0;
    if(!j->depth && j->root)return -1;
    net_json_frame_t *f=j->depth?&j->stack[j->depth-1U]:NULL;
    if(f) {
        unsigned e=f->expect;
        if((c=='}'&&(e==O_KEY_END||e==O_COMMA)) || (c==']'&&(e==A_VALUE_END||e==A_COMMA))) {
            j->tokens[f->token].end=j->at+1U;j->tokens[f->token].next=j->count;
            --j->depth;value_done(j);return 0;
        }
        if(e==O_COLON) {if(c!=':')return -1;f->expect=O_VALUE;return 0;}
        if(e==O_COMMA||e==A_COMMA){if(c!=',')return -1;f->expect=e==O_COMMA?O_KEY:A_VALUE;return 0;}
        if(e==O_KEY||e==O_KEY_END) {
            if(c!='"'||token(j,NJ_STRING|0x80U))return -1;
            j->lex=L_STRING;return 0;
        }
    }
    if(c=='{'||c=='[') {
        if(j->depth==16||token(j,c=='{'?NJ_OBJECT:NJ_ARRAY))return -1;
        j->stack[j->depth].token=j->current;
        j->stack[j->depth++].expect=c=='{'?O_KEY_END:A_VALUE_END;
    } else if(c=='"') {if(token(j,NJ_STRING))return -1;j->lex=L_STRING;}
    else if(c=='-'||(c>='0'&&c<='9')) {
        if(token(j,NJ_NUMBER))return -1;
        j->lex=L_NUMBER;j->number=0;
        if(c!='-'&&number_byte(j,c))return -1;
    } else if(c=='t'||c=='f'||c=='n') {
        if(token(j,c=='n'?NJ_NULL:NJ_BOOL))return -1;
        j->lex=c=='t'?L_TRUE:c=='f'?L_FALSE:L_NULL;j->literal_at=1;
    } else return -1;
    return 0;
}
void net_json_init(net_json_t *j,const uint8_t *source)
{
    /* Token and output storage need no clearing: count/length own those bytes. */
    j->source=source;j->count=j->at=j->current=j->high=j->literal_at=0;
    j->depth=j->root=j->bad=j->complete=j->lex=j->escape=j->digits=j->utf8_left=0;
    j->result=NJ_NONE;j->value_length=j->value_type=0;
}
void net_json_feed(net_json_t *j,size_t available,int final,size_t budget)
{
    if(j->bad||j->complete)return;
    if(available>NET_JSON_BYTES){j->bad=1;return;}
    while(j->at<available&&budget--) {
        if(consume(j,j->source[j->at])){j->bad=1;return;}
        ++j->at;
    }
    if(final&&j->at==available) {
        if(j->lex==L_NUMBER&&(j->number==1||j->number==2||j->number==4||j->number==7)) {
            j->tokens[j->current].end=j->at;j->lex=L_IDLE;value_done(j);
        }
        if(j->lex||j->depth||!j->root)j->bad=1;else j->complete=1;
    }
}
/* Input has already passed validation. Decode one code point, including escapes. */
static unsigned decoded(const uint8_t *s,uint16_t *at,uint8_t out[4])
{
    uint32_t cp;
    uint8_t c=s[(*at)++];
    if(c!='\\'){out[0]=c;return 1;}
    c=s[(*at)++];
    if(c!='u') {
        const char *p=strchr("\"\\/bfnrt",c);
        out[0]=(const uint8_t[]){'"','\\','/',8,12,10,13,9}[p-"\"\\/bfnrt"];return 1;
    }
    cp=0;for(unsigned i=0;i<4;++i)cp=(cp<<4)|(unsigned)hex(s[(*at)++]);
    if(cp>=0xD800&&cp<=0xDBFF) {
        uint32_t low=0;*at+=2;
        for(unsigned i=0;i<4;++i)low=(low<<4)|(unsigned)hex(s[(*at)++]);
        cp=0x10000+((cp-0xD800)<<10)+(low-0xDC00);
    }
    if(cp<0x80){out[0]=(uint8_t)cp;return 1;}
    if(cp<0x800){out[0]=(uint8_t)(0xC0|(cp>>6));out[1]=(uint8_t)(0x80|(cp&63));return 2;}
    if(cp<0x10000){out[0]=(uint8_t)(0xE0|(cp>>12));out[1]=(uint8_t)(0x80|((cp>>6)&63));out[2]=(uint8_t)(0x80|(cp&63));return 3;}
    out[0]=(uint8_t)(0xF0|(cp>>18));out[1]=(uint8_t)(0x80|((cp>>12)&63));out[2]=(uint8_t)(0x80|((cp>>6)&63));out[3]=(uint8_t)(0x80|(cp&63));return 4;
}
void net_json_lookup(net_json_t *j,const uint8_t *path,size_t length,int document)
{
    j->value_length=j->value_type=0;
    if(!document){j->result=NJ_NO_DOCUMENT;return;}
    if(j->bad||!j->complete){j->result=NJ_BAD_DOCUMENT;return;}
    if(length>NET_JSON_PATH||(length&&path[0]!='/')){j->result=NJ_BAD_PATH;return;}
    for(size_t i=0;i<length;++i) {
        if(!path[i]){j->result=NJ_BAD_PATH;return;}
        if(path[i]=='~'&&(++i==length||(path[i]!='0'&&path[i]!='1'))){j->result=NJ_BAD_PATH;return;}
    }
    memcpy(j->path,path,length);j->path[length]=0;
    j->path_at=0;j->selected=0;j->lookup_stage=0;j->result=NJ_PENDING;
}
void net_json_poll(net_json_t *j,size_t budget)
{
    while(j->result==NJ_PENDING&&budget) {
        net_json_token_t *t=&j->tokens[j->selected];
        if(j->lookup_stage==0) {
            --budget;
            if(!j->path[j->path_at]) {
                j->value_type=t->type;j->decode_at=t->start+(t->type==NJ_STRING);
                j->lookup_stage=3;continue;
            }
            ++j->path_at;j->part_length=0;
            j->lookup_stage=4;continue;
        }
        if(j->lookup_stage==4) {
            --budget;
            uint8_t c=(uint8_t)j->path[j->path_at];
            if(c&&c!='/') {
                ++j->path_at;
                if(c=='~')c=j->path[j->path_at++]=='1'?'/':'~';
                j->part[j->part_length++]=(char)c;continue;
            }
            j->candidate=j->selected+1U;j->array_at=0;j->part_at=0;j->compare_at=0;j->mismatch=0;
            if(t->type==NJ_ARRAY) {
                uint32_t index=0;
                if(!j->part_length||(j->part_length>1&&j->part[0]=='0')){j->result=NJ_BAD_PATH;continue;}
                for(unsigned i=0;i<j->part_length;++i) {
                    unsigned d=(unsigned)(j->part[i]-'0');
                    if(d>9||index>(UINT32_MAX-d)/10U){j->result=NJ_BAD_PATH;break;}index=index*10U+d;
                }
                j->index=index;
            } else if(t->type!=NJ_OBJECT){j->result=NJ_MISSING;continue;}
            j->lookup_stage=1;continue;
        }
        if(j->lookup_stage==1) {
            --budget;
            if(j->candidate>=t->next){j->result=NJ_MISSING;continue;}
            net_json_token_t *key=&j->tokens[j->candidate];
            if(t->type==NJ_ARRAY) {
                if(j->array_at++==j->index){j->selected=j->candidate;j->lookup_stage=0;}
                else j->candidate=key->next;
            } else {j->decode_at=key->start+1U;j->compare_at=0;j->mismatch=0;j->lookup_stage=2;}
            continue;
        }
        if(j->lookup_stage==2) {
            net_json_token_t *key=&j->tokens[j->candidate];
            if(j->decode_at<key->end-1U) {
                uint8_t bytes[4];uint16_t before=j->decode_at;
                unsigned n=decoded(j->source,&j->decode_at,bytes);
                size_t cost=j->decode_at-before;budget=budget>cost?budget-cost:0;
                for(unsigned k=0;k<n;++k) {
                    if(j->compare_at>=j->part_length||bytes[k]!=(uint8_t)j->part[j->compare_at])j->mismatch=1;
                    ++j->compare_at;
                }
            } else if(!j->mismatch&&j->compare_at==j->part_length) {
                j->selected=j->candidate+1U;j->lookup_stage=0;
            } else {j->candidate=j->tokens[j->candidate+1U].next;j->lookup_stage=1;}
            continue;
        }
        if(j->lookup_stage==3) {
            uint16_t end=t->end-(t->type==NJ_STRING);
            if(j->decode_at==end){j->result=NJ_FOUND;continue;}
            uint8_t bytes[4];uint16_t before=j->decode_at;
            unsigned n;
            if(t->type==NJ_STRING)n=decoded(j->source,&j->decode_at,bytes);
            else {bytes[0]=j->source[j->decode_at++];n=1;}
            size_t cost=j->decode_at-before;budget=budget>cost?budget-cost:0;
            memcpy(j->value+j->value_length,bytes,n);j->value_length+=(uint16_t)n;
        }
    }
}
