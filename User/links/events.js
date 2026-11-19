/* Each navigation owns its document, timers and pending promise callbacks. */
(function (window) {
    var timers = [], jobs = [], sequence = 0;
    function timer(repeat, callback, delay, args) {
        if (typeof callback !== 'function') throw new TypeError('Expected a timer callback');
        if (timers.length >= 128) throw new RangeError('Timer limit');
        delay = Math.max(1, Math.min(2147483647, Number(delay) || 0));
        var item = {id: ++sequence, fn: callback, due: __now() + delay,
                    interval: repeat ? delay : 0, args: args};
        timers.push(item);
        return item.id;
    }
    window.setTimeout = function (f, ms) {return timer(false, f, ms, [].slice.call(arguments, 2));};
    window.setInterval = function (f, ms) {return timer(true, f, ms, [].slice.call(arguments, 2));};
    window.clearTimeout = window.clearInterval = function (id) {
        timers = timers.filter(function (t) {return t.id !== id;});
    };
    function queue(fn) {
        if (jobs.length >= 256) throw new RangeError('Pending job limit');
        jobs.push(fn);
    }
    function Promise(executor) {
        var state = 0, value, listeners = [], locked = false;
        function settle(kind, x) {
            if (state) return;
            state = kind; value = x;
            listeners.forEach(queue); listeners = [];
        }
        var self = this;
        function resolve(x) {
            if (x === self) {settle(2, new TypeError('Promise cycle')); return;}
            try {
                var then = x && x.then;
                if (typeof then === 'function') {
                    var done = false;
                    then.call(x, function (v) {if (!done) {done=true; resolve(v);}},
                                  function (e) {if (!done) {done=true; settle(2,e);}});
                } else settle(1,x);
            } catch (e) {settle(2,e);}
        }
        this.then = function (ok, bad) {
            return new Promise(function (res, rej) {
                function run() {
                    var fn = state === 1 ? ok : bad;
                    if (typeof fn !== 'function') {(state === 1 ? res : rej)(value); return;}
                    try {res(fn(value));} catch (e) {rej(e);}
                }
                if (state) queue(run); else listeners.push(run);
            });
        };
        try {
            executor(function (x) {if (!locked) {locked=true;resolve(x);}},
                     function (e) {if (!locked) {locked=true;settle(2,e);}});
        } catch (e) {if (!locked) settle(2,e);}
    }
    Promise.prototype.catch = function (bad) {return this.then(null,bad);};
    Promise.resolve = function (x) {return new Promise(function (resolve) {resolve(x);});};
    window.Promise = Promise;
    window.fetch = function (url, options) {
        return Promise.resolve().then(function () {
            if (options && (String(options.method || 'GET').toUpperCase() !== 'GET' || options.body || options.headers))
                throw new TypeError('Only a same-origin GET is supported');
            var r = __fetch(String(url)), consumed = false;
            function read(json) {
                return Promise.resolve().then(function () {
                    if (consumed) throw new TypeError('Response already consumed');
                    consumed = true;
                    return json ? JSON.parse(r.body) : r.body;
                });
            }
            return {status:r.status,ok:r.status>=200 && r.status<300,
                    text:function () {return read(false);},json:function () {return read(true);}};
        });
    };
    var location = {
        toString: function(){return __location();},
        assign: function(url){__location(String(url),false);},
        replace: function(url){__location(String(url),true);},
        reload: function(){__location(__location(),true);}
    };
    Object.defineProperty(location,'href',{get:function(){return __location();},set:location.assign});
    var parts=/^([a-z]+:)\/\/([^/?#]*)([^?#]*)(\?[^#]*)?(#.*)?$/.exec(__location());
    if (parts) {
        location.protocol=parts[1];location.host=parts[2];location.hostname=parts[2].split(':')[0];
        location.port=parts[2].split(':')[1]||'';location.pathname=parts[3]||'/';
        location.search=parts[4]||'';location.hash=parts[5]||'';location.origin=parts[1]+'//'+parts[2];
    }
    Object.defineProperty(window,'location',{get:function(){return location;},set:location.assign});
    var started=__now();
    window.performance={now:function(){return __now()-started;}};
    window.navigator={userAgent:'Mozilla/5.0 (compatible; Links/2.30; LixinOS aarch64)',
                      platform:'LixinOS aarch64',language:'en',languages:['en'],onLine:true};
    window.atob=function(s){var bytes=Duktape.dec('base64',String(s)),out='';for(var i=0;i<bytes.length;i++)out+=String.fromCharCode(bytes[i]);return out;};
    window.btoa=function(s){
        s=String(s);var data=new Uint8Array(s.length);
        for(var i=0;i<s.length;i++){var c=s.charCodeAt(i);if(c>255)throw new TypeError('Expected Latin-1');data[i]=c;}
        return Duktape.enc('base64',data);
    };
    window.self=window.top=window.parent=window;
    window.console={log:function(){},warn:function(){},error:function(){},info:function(){}};
    window.__bindDOM=function(impl) {
        var parser=new impl.HTMLParser(location.href);
        var doc=parser.document(), view=new impl.Window(doc);
        Object.keys(impl).forEach(function(k){window[k]=impl[k];});
        ['addEventListener','removeEventListener','dispatchEvent'].forEach(function(k){
            window[k]=view[k].bind(view);
        });
        Object.defineProperty(doc,'location',{get:function(){return location;},set:location.assign});
        Object.defineProperty(doc,'cookie',{get:__cookie,set:__cookie});
        Object.defineProperty(doc,'defaultView',{value:window});
        window._dispatchEvent=view._dispatchEvent.bind(view);
        window.document=doc;
        var currentScript=null;
        Object.defineProperty(doc,'currentScript',{get:function(){return currentScript;}});
        window.__feed=function(s,eof){
            parser.parse(s,eof);
            if (!eof) {
                var list=doc.getElementsByTagName('script');
                currentScript=list[list.length-1]||null;
            }
        };
        window.__endScript=function(){currentScript=null;};
        function evaluateScript(script,code) {
            var previous=currentScript;
            currentScript=script;
            try {(0,eval)(code);} finally {currentScript=previous;}
        }
        Object.defineProperty(doc,'write',{value:function(){parser.parse([].join.call(arguments,''),false);}});
        Object.defineProperty(doc,'writeln',{value:function(){doc.write([].join.call(arguments,'')+'\n');}});
        var scripts=[], createElement=doc.createElement;
        Object.defineProperty(doc,'createElement',{value:function(tag){
            var el=createElement.apply(this,arguments);
            if (el.localName==='script') {
                if (scripts.length>=128) throw new RangeError('Page script limit');
                scripts.push(el);
            }
            return el;
        }});
        doc.mutationHandler=function(change){
            __dirty();
            var script=change.type===6?change.node:change.type===2?change.target:null;
            if (!script || script.localName!=='script' || scripts.indexOf(script)===-1) return;
            var type=(script.getAttribute('type')||'').toLowerCase();
            if (type && type!=='text/javascript' && type!=='application/javascript') return;
            var ref=script.getAttribute('src'), code=script.textContent;
            if (!ref && !code) return;
            scripts.splice(scripts.indexOf(script),1);
            function complete(ok){
                var event=doc.createEvent('Event');
                event.initEvent(ok?'load':'error',false,false);
                script.dispatchEvent(event);
            }
            if (ref) {
                queue(function(){
                    try {
                        var response=__fetch(ref);
                        if (response.status<200 || response.status>=300) {complete(false);return;}
                        evaluateScript(script,response.body);
                    } catch(e){complete(false);return;}
                    complete(true);
                });
            } else evaluateScript(script,code);
        };
        window.__serialize=function(){__title(doc.title||'');return doc.serialize();};
        window.__ready=function(){
            view.onload=window.onload;
            doc.close();
        };
        window.__click=function(id,tag,index){
            var target=id?doc.getElementById(id):tag?doc.getElementsByTagName(tag)[index]:null;
            if (!target) return false;
            var event=doc.createEvent('MouseEvents');
            event.initMouseEvent('click',true,true,window,1,0,0,0,0,false,false,false,false,0,null);
            return !target.dispatchEvent(event);
        };
    };
    window.__tick=function(){
        var now=__now(), t;
        for (var i=0;i<timers.length;i++) if (timers[i].due<=now) {
            t=timers[i];
            if (t.interval) t.due=now+t.interval; else timers.splice(i,1);
            t.fn.apply(window,t.args); break;
        }
        for (var j=0;j<64 && jobs.length;j++) jobs.shift()();
    };
})(this);
var window=this;
