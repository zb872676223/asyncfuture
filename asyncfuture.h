#pragma once
#include <QFuture>
#include <QMetaMethod>
#include <QPointer>
#include <QThread>
#include <QFutureWatcher>

namespace AsyncFuture {

/* Naming Convention
 *
 * typename T - The type of observable QFuture
 * typename R - The return type of callback
 */

namespace Private {

// Value is a wrapper of data structure which could contain <void> type.
// AsyncFuture do not use QVariant because it need user to register before use.
template <typename R>
class Value {
public:
    Value() {
    }

    Value(R&& v) : value(v){
    }

    Value(R* v) : value(*v) {
    }

    Value(QFuture<R> future) {
        value = future.result();
    }

    R value;
};

template <>
class Value<void> {
public:
    Value() {
    }

    Value(void*) {
    }

    Value(QFuture<void> future) {
        Q_UNUSED(future);
    }
};

template <typename T>
class DeferredFuture : public QObject, public QFutureInterface<T>{
public:
    DeferredFuture(QObject* parent = 0): QObject(parent),
                                         QFutureInterface<T>(QFutureInterface<T>::Running),
                                         autoDelete(false),
                                         resolved(false) {

    }

    ~DeferredFuture() {
        cancel();
    }

    void complete() {
        if (resolved) {
            return;
        }
        resolved = true;
        QFutureInterface<T>::reportFinished();
    }

    template <typename R>
    void complete(R value) {
        if (resolved) {
            return;
        }
        resolved = true;
        reportResult(value);
        QFutureInterface<T>::reportFinished();

        if (autoDelete) {
            deleteLater();
        }
    }

    template <typename R>
    void complete(Value<R> value) {
        if (resolved) {
            return;
        }
        resolved = true;
        reportResult(value);
        QFutureInterface<T>::reportFinished();

        if (autoDelete) {
            deleteLater();
        }
    }

    void complete(QFuture<T> future) {

        QFutureWatcher<T> *watcher = new QFutureWatcher<T>(this);
        watcher->setFuture(future);
        connect(watcher, &QFutureWatcher<T>::finished,
                watcher, [=]() {
            watcher->disconnect();
            watcher->deleteLater();

            if (resolved) {
                return;
            }
            resolved = true;
            Value<T> value(future);
            this->reportResult(value);
            this->reportFinished();

            if (autoDelete) {
                this->deleteLater();
            }
        });
    }


    void cancel() {
        if (resolved) {
            return;
        }
        resolved = true;
        QFutureInterface<T>::reportCanceled();
        QFutureInterface<T>::reportFinished();

        if (autoDelete) {
            this->deleteLater();
        }
    }

    template <typename Member>
    void cancel(QObject* sender, Member member) {

        QObject::connect(sender, member,
                         this, [=]() {
            this->cancel();
        });
    }

    bool autoDelete;
    bool resolved;

private:

    void reportResult(Value<void> &value) {
        Q_UNUSED(value);
    }

    template <typename R>
    void reportResult(R& value) {
        QFutureInterface<T>::reportResult(value);
    }

    template <typename R>
    void reportResult(Value<R>& value) {
        QFutureInterface<T>::reportResult(value.value);
    }

};

template <typename ARG>
class Proxy : public QObject {
public:
    Proxy(QObject* parent) : QObject(parent) {
    }

    QVector<int> parameterTypes;
    std::function<void(Value<ARG>)> callback;
    QMetaObject::Connection conn;
    QPointer<QObject> sender;

    template <typename Method>
    void bind(QObject* source, Method pointToMemberFunction) {
        sender = source;

        const int memberOffset = QObject::staticMetaObject.methodCount();

        QMetaMethod method = QMetaMethod::fromSignal(pointToMemberFunction);

        parameterTypes = QVector<int>(method.parameterCount());

        for (int i = 0 ; i < method.parameterCount() ; i++) {
            parameterTypes[i] = method.parameterType(i);
        }

        conn = QMetaObject::connect(source, method.methodIndex(), this, memberOffset, Qt::QueuedConnection, 0);

        if (!conn) {
            qWarning() << "AsyncFuture::Private::Proxy: Failed to bind signal";
        }
    }

    int qt_metacall(QMetaObject::Call _c, int _id, void **_a) {
        int methodId = QObject::qt_metacall(_c, _id, _a);

        if (methodId < 0) {
            return methodId;
        }

        if (_c == QMetaObject::InvokeMetaMethod) {
            if (methodId == 0) {
                sender->disconnect(conn);
                Value<ARG> v;
                if (parameterTypes.count() > 0) {

                    Value<ARG> value(reinterpret_cast<ARG*>(_a[1]));
                    v = value;
                }

                callback(v);
            }
        }
        return methodId;
    }
};

// function_traits: Source: http://stackoverflow.com/questions/7943525/is-it-possible-to-figure-out-the-parameter-type-and-return-type-of-a-lambda

template <typename T>
struct function_traits
        : public function_traits<decltype(&T::operator())>
{};

template <typename ClassType, typename ReturnType, typename... Args>
struct function_traits<ReturnType(ClassType::*)(Args...) const>
// we specialize for pointers to member function
{
    enum { arity = sizeof...(Args) };
    // arity is the number of arguments.

    typedef ReturnType result_type;

    template <size_t i>
    struct arg
    {
        typedef typename std::tuple_element<i, std::tuple<Args...>>::type type;
        // the i-th argument is equivalent to the i-th tuple element of a tuple
        // composed of those arguments.
    };
};

template <typename T>
struct signal_traits {
};

template <typename R, typename C>
struct signal_traits<R (C::*)()> {
    typedef void result_type;
};

template <typename R, typename C, typename ARG1>
struct signal_traits<R (C::*)(ARG1)> {
    typedef ARG1 result_type;
};


template <typename T>
struct future_traits {
    enum {
        is_future = 0
    };
};


template <template <typename> class C, typename T>
struct future_traits<C <T> >
{
    enum {
        is_future = 0
    };
};

template <typename T>
struct future_traits<QFuture<T> >{
    enum {
        is_future = 1
    };
    typedef T arg_type;
};

template <typename Functor, typename T>
typename std::enable_if<!(std::is_same<T, void>::value || Private::function_traits<Functor>::arity != 1) ,
Value<typename Private::function_traits<Functor>::result_type>>::type
    invoke(Functor functor, QFuture<T> future) {
    Value<typename Private::function_traits<Functor>::result_type> value(functor(future));
    return value;
}

template <typename Functor, typename T>
typename std::enable_if<(std::is_same<T, void>::value || Private::function_traits<Functor>::arity != 1) ,
Value<typename Private::function_traits<Functor>::result_type>>::type
    invoke(Functor functor, QFuture<T> future) {
    Q_UNUSED(future);
    static_assert(Private::function_traits<Functor>::arity == 0, "Your callback should not take any argument because the observed type is QFuture<void>");
    Value<typename Private::function_traits<Functor>::result_type> value(functor());
    return value;
}

template <typename Functor, typename T>
typename std::enable_if<!(std::is_same<T, void>::value || Private::function_traits<Functor>::arity != 1) ,
void>::type
voidInvoke(Functor functor, QFuture<T> future) {
    functor(future);
}

template <typename Functor, typename T>
typename std::enable_if<(std::is_same<T, void>::value || Private::function_traits<Functor>::arity != 1) ,
void>::type
voidInvoke(Functor functor, QFuture<T> future) {
    Q_UNUSED(future);
    functor();
}

template <typename Functor, typename T>
typename std::enable_if<!std::is_same<typename Private::function_traits<Functor>::result_type, void>::value,
Value<typename Private::function_traits<Functor>::result_type>>::type
run(Functor functor, QFuture<T> future) {
    auto value = invoke(functor,future);
    return value;
}

template <typename Functor, typename T>
typename std::enable_if<std::is_same<typename Private::function_traits<Functor>::result_type, void>::value,
Value<void>>::type
run(Functor functor, QFuture<T> future) {
    voidInvoke(functor, future);
    return Value<void>();
}

template <typename F>
void nextTick(F f) {
    QObject tmp;
    QObject::connect(&tmp, &QObject::destroyed,
                     QThread::currentThread(),
                     f,
                     Qt::QueuedConnection);
}

} // End of Private Namespace

template <typename T>
class Observable {
protected:
    QFuture<T> m_future;

public:

    Observable() {

    }

    Observable(QFuture<T> future) {
        m_future = future;
    }

    QFuture<T> future() const {
        return m_future;
    }

    template <typename Functor>
    typename std::enable_if< !Private::future_traits<typename Private::function_traits<Functor>::result_type>::is_future,
    Observable<typename Private::function_traits<Functor>::result_type>
    >::type
    context(QObject* contextObject, Functor functor)  {
        return context<typename Private::function_traits<Functor>::result_type,
                       typename Private::function_traits<Functor>::result_type
                >(contextObject, functor);
    }

    template <typename Functor>
    typename std::enable_if< Private::future_traits<typename Private::function_traits<Functor>::result_type>::is_future,
    Observable<typename Private::future_traits<typename Private::function_traits<Functor>::result_type>::arg_type>
    >::type
    context(QObject* contextObject, Functor functor)  {
        /* For functor return QFuture */
        return context<typename Private::future_traits<typename Private::function_traits<Functor>::result_type>::arg_type,
                       typename Private::function_traits<Functor>::result_type
                >(contextObject, functor);
    }

    template <typename Functor1, typename Functor2>
    void subscribe(Functor1 onCompleted,
                   Functor2 onCancelled) {
        QFutureWatcher<T> *watcher = new QFutureWatcher<T>();

        watcher->setFuture(m_future);

        QObject::connect(watcher,
                         &QFutureWatcher<T>::finished,
                         QThread::currentThread(),
                         [=](){
            watcher->disconnect();
            watcher->deleteLater();

            Private::run(onCompleted, m_future);
        });

        QObject::connect(watcher,
                         &QFutureWatcher<T>::canceled,
                         QThread::currentThread(),
                         [=](){
            watcher->disconnect();
            watcher->deleteLater();

            Private::run(onCancelled, m_future);
        });
    }

    template <typename Functor1>
    void subscribe(Functor1 onCompleted) {
        subscribe(onCompleted, [](){});
    }

private:
    template <typename ObservableType, typename RetType, typename Functor>
    Observable<ObservableType> context(QObject* contextObject, Functor functor)  {

        static_assert(Private::function_traits<Functor>::arity <= 1, "context(): Callback should take not more than one parameter");

        auto defer = new Private::DeferredFuture<ObservableType> ();
        defer->autoDelete = true;

        auto watcher = new QFutureWatcher<T>(defer);
        watcher->setFuture(m_future);

        QObject::connect(watcher, &QFutureWatcher<T>::finished,
                         watcher, [=](){
            watcher->disconnect();
            Private::Value<RetType> value = Private::run(functor, m_future);
            defer->complete(value);
        });

        defer->cancel(contextObject, &QObject::destroyed);

        return Observable<ObservableType>(defer->future());
    }

};

template <typename T>
class Defer : public Observable<T> {

public:
    Defer() : Observable<T>(), defer(new Private::DeferredFuture<T>())  {
        this->m_future = defer->future();
    }

    void complete(T value)
    {
        this->defer->complete(value);
    }

    void cancel() {
        defer->cancel();
    }

private:
    QSharedPointer<Private::DeferredFuture<T> > defer;
};

template<>
class Defer<void> : public Observable<void> {

public:
    Defer() : Observable<void>(), defer(new Private::DeferredFuture<void>())  {
        this->m_future = defer->future();
    }

    void complete() {
        defer->complete();
    }

    void cancel() {
        defer->cancel();
    }

private:
    QSharedPointer<Private::DeferredFuture<void> > defer;
};

template <typename T>
static Observable<T> observe(QFuture<T> future) {
    return Observable<T>(future);
}

template <typename Member>
static auto observe(QObject* object, Member pointToMemberFunction)
-> Observable< typename Private::signal_traits<Member>::result_type> {

    typedef typename Private::signal_traits<Member>::result_type RetType;

    auto defer = new Private::DeferredFuture<RetType>();

    auto proxy = new Private::Proxy<RetType>(defer);

    defer->autoDelete = true;

    defer->cancel(object, &QObject::destroyed);

    proxy->bind(object, pointToMemberFunction);
    proxy->callback = [=](Private::Value< RetType> value) {
        defer->complete(value);
        proxy->deleteLater();
    };

    Observable< typename Private::signal_traits<Member>::result_type> observer(defer->future());
    return observer;
}

template <typename T>
auto defer() -> Defer<T> {
    return Defer<T>();
}

}
