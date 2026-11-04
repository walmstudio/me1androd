import json,re,sys,os
D=os.path.dirname(os.path.abspath(__file__))
J=json.load(open(D+'/java_keys.json')); S=json.load(open(D+'/schema_strs.json'))
src=open(D+'/../src/com/me1port/launcher/L.java').read()
fr=src[src.index('FRAGS = {')+9:src.index('};',src.index('FRAGS = {'))]
F=[json.loads('"'+m+'"') for m in re.findall(r'"((?:[^"\\]|\\.)*)"',fr)]
X=["Язык интерфейса","Язык текста в игре","Переключить и язык текста в игре на ","? Нужны файлы локализации этого языка в папке игры.","Да","Нет"]
KEYS={}
for p,arr in (('J',J),('S',S),('F',F),('X',X)):
    for i,k in enumerate(arr): KEYS[f'{p}{i}']=k
FALLBACK=set([f'J{i}' for i in list(range(192,202))+list(range(203,227))+list(range(228,231))+list(range(232,238))+list(range(239,259))+list(range(260,264))+list(range(276,288))])
def dec(v): return re.sub(r'\\(n|\\)',lambda m:'\n' if m.group(1)=='n' else '\\',v)
def ws(k):
    a=len(k)-len(k.lstrip()); b=len(k.rstrip()); return k[:a],k[b:]
spec=re.compile(r'%[-0-9.]*[sdfg]')
def build(code):
    tr={}
    for ln in open(f'{D}/tr/{code}.txt',encoding='utf-8'):
        ln=ln.rstrip('\n')
        if not ln.strip(): continue
        i,v=ln.split('|',1)
        if i not in KEYS: print(code,'bad id',i); continue
        k=KEYS[i]; pre,suf=ws(k); v=pre+dec(v).strip()+suf
        if spec.findall(k)!=spec.findall(v): print(code,'spec mismatch',i,repr(v))
        if k in tr and tr[k]!=v: print(code,'dup differs',i,repr(tr[k]),repr(v))
        tr.setdefault(k,v)
    need=[i for i in KEYS if (code=='en' or i not in FALLBACK)]
    miss=[i for i in need if KEYS[i] not in tr]
    if miss: print(code,'missing',len(miss),miss[:40])
    os.makedirs(D+'/../assets/i18n',exist_ok=True)
    json.dump(tr,open(f'{D}/../assets/i18n/{code}.json','w',encoding='utf-8'),ensure_ascii=False,indent=0,sort_keys=True)
    print(code,len(tr),'keys')
for c in sys.argv[1:]: build(c)
