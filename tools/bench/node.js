const T=(n,f)=>{const t=Date.now(); f(); return n+' '+(Date.now()-t)};
const out=[];
out.push(T('json',()=>{let o=[];for(let i=0;i<60000;i++)o.push({id:i,name:'item'+i,tags:['a','b','c'],v:i/3});for(let k=0;k<4;k++){const s=JSON.stringify(o);JSON.parse(s);}}));
out.push(T('regex',()=>{let s='';for(let i=0;i<20000;i++)s+='foo'+i+' bar_'+i+'@example.com, ';let c=0;for(let k=0;k<10;k++)c+=(s.match(/[a-z_0-9]+@[a-z]+\.com/g)||[]).length;}));
out.push(T('class',()=>{class P{constructor(x){this.x=x}get d(){return this.x*2}} let s=0;for(let i=0;i<3e6;i++){const p=new P(i);s+=p.d}}));
out.push(T('map',()=>{const m=new Map();for(let i=0;i<400000;i++)m.set('k'+i,i);let s=0;for(const [k,v] of m)s+=v;}));
out.push(T('typed',()=>{const a=new Float64Array(1e6);for(let k=0;k<10;k++)for(let i=0;i<a.length;i++)a[i]=Math.sin(i)*k;}));
console.log(out.join(' | '));
