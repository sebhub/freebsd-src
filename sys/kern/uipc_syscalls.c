/*-
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright (c) 1982, 1986, 1989, 1990, 1993
 *	The Regents of the University of California.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 *	@(#)uipc_syscalls.c	8.4 (Berkeley) 2/21/94
 */

#ifdef __rtems__
#include <sys/file.h>
#endif /* __rtems__ */
#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

#include "opt_capsicum.h"
#include "opt_inet.h"
#include "opt_inet6.h"
#include "opt_ktrace.h"

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/capsicum.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/sysproto.h>
#include <sys/malloc.h>
#include <sys/filedesc.h>
#include <sys/proc.h>
#include <sys/filio.h>
#include <sys/jail.h>
#include <sys/mbuf.h>
#include <sys/protosw.h>
#include <sys/rwlock.h>
#include <sys/socket.h>
#include <sys/socketvar.h>
#include <sys/syscallsubr.h>
#include <sys/sysent.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <sys/unpcb.h>
#ifdef KTRACE
#include <sys/ktrace.h>
#endif
#ifdef COMPAT_FREEBSD32
#include <compat/freebsd32/freebsd32_util.h>
#endif

#include <net/vnet.h>

#include <security/audit/audit.h>
#include <security/mac/mac_framework.h>

static int sendit(struct thread *td, int s, struct msghdr *mp, int flags);
static int recvit(struct thread *td, int s, struct msghdr *mp, void *namelenp);

#ifndef __rtems__
static int accept1(struct thread *td, int s, struct sockaddr *uname,
		   socklen_t *anamelen, int flags);
static int getsockname1(struct thread *td, struct getsockname_args *uap,
			int compat);
static int getpeername1(struct thread *td, struct getpeername_args *uap,
			int compat);
#else /* __rtems__ */
struct getsockaddr_sockaddr {
	struct sockaddr	header;
	char		data[SOCK_MAXADDRLEN - sizeof(struct sockaddr)];
} __aligned(sizeof(long));

static int kern_getsockname(struct thread *, int, struct sockaddr **,
    socklen_t *);
static int kern_listen(struct thread *, int, int);
static int kern_setsockopt(struct thread *, int, int, int, const void *,
    enum uio_seg, socklen_t);
static int kern_shutdown(struct thread *, int, int);
static int kern_socket(struct thread *, int, int, int);
static int kern_socketpair(struct thread *, int, int, int, int *);
#endif /* __rtems__ */
static int sockargs(struct mbuf **, char *, socklen_t, int);

#ifndef __rtems__
/*
 * Convert a user file descriptor to a kernel file entry and check if required
 * capability rights are present.
 * If required copy of current set of capability rights is returned.
 * A reference on the file entry is held upon returning.
 */
int
getsock_cap(struct thread *td, int fd, cap_rights_t *rightsp,
    struct file **fpp, u_int *fflagp, struct filecaps *havecapsp)
{
	struct file *fp;
	int error;

	error = fget_cap(td, fd, rightsp, &fp, havecapsp);
	if (error != 0)
		return (error);
	if (fp->f_type != DTYPE_SOCKET) {
		fdrop(fp, td);
		if (havecapsp != NULL)
			filecaps_free(havecapsp);
		return (ENOTSOCK);
	}
	if (fflagp != NULL)
		*fflagp = fp->f_flag;
	*fpp = fp;
	return (0);
}
#else /* __rtems__ */
static int
rtems_bsd_getsock(int fd, struct file **fpp, u_int *fflagp)
{
	struct file *fp;
	int error;

	if ((uint32_t) fd < rtems_libio_number_iops) {
		unsigned int flags;

		fp = rtems_libio_iop(fd);
		flags = rtems_libio_iop_hold(fp);
		if ((flags & LIBIO_FLAGS_OPEN) == 0) {
			rtems_libio_iop_drop(fp);
			fp = NULL;
			error = EBADF;
		} else if (fp->pathinfo.handlers != &socketops) {
			rtems_libio_iop_drop(fp);
			fp = NULL;
			error = ENOTSOCK;
		} else {
			if (fflagp != NULL) {
				*fflagp = rtems_bsd_libio_flags_to_fflag(
				    fp->flags);
			}

			error = 0;
		}
	} else {
		fp = NULL;
		error = EBADF;
	}

	*fpp = fp;

	return (error);
}

#define	getsock_cap(td, fd, rights, fpp, fflagp, havecapsp) \
    rtems_bsd_getsock(fd, fpp, fflagp)
#endif /* __rtems__ */

/*
 * System call interface to the socket abstraction.
 */
#if defined(COMPAT_43)
#define COMPAT_OLDSOCK
#endif

#ifdef __rtems__
static
#endif /* __rtems__ */
int
sys_socket(struct thread *td, struct socket_args *uap)
{

	return (kern_socket(td, uap->domain, uap->type, uap->protocol));
}

int
kern_socket(struct thread *td, int domain, int type, int protocol)
{
	struct socket *so;
	struct file *fp;
	int fd, error, oflag, fflag;

	AUDIT_ARG_SOCKET(domain, type, protocol);

	oflag = 0;
	fflag = 0;
	if ((type & SOCK_CLOEXEC) != 0) {
		type &= ~SOCK_CLOEXEC;
		oflag |= O_CLOEXEC;
	}
	if ((type & SOCK_NONBLOCK) != 0) {
		type &= ~SOCK_NONBLOCK;
		fflag |= FNONBLOCK;
	}

#ifdef MAC
	error = mac_socket_check_create(td->td_ucred, domain, type, protocol);
	if (error != 0)
		return (error);
#endif
	error = falloc(td, &fp, &fd, oflag);
	if (error != 0)
		return (error);
	/* An extra reference on `fp' has been held for us by falloc(). */
	error = socreate(domain, &so, type, protocol, td->td_ucred, td);
	if (error != 0) {
		fdclose(td, fp, fd);
	} else {
		finit(fp, FREAD | FWRITE | fflag, DTYPE_SOCKET, so, &socketops);
		if ((fflag & FNONBLOCK) != 0)
			(void) fo_ioctl(fp, FIONBIO, &fflag, td->td_ucred, td);
		td->td_retval[0] = fd;
	}
#ifndef __rtems__
	fdrop(fp, td);
#endif /* __rtems__ */
	return (error);
}
#ifdef __rtems__
int
socket(int domain, int type, int protocol)
{
	struct thread *td = rtems_bsd_get_curthread_or_null();
	struct socket_args ua = {
		.domain = domain,
		.type = type,
		.protocol = protocol
	};
	int error;

	if (td != NULL) {
		error = sys_socket(td, &ua);
	} else {
		error = ENOMEM;
	}

	if (error == 0) {
		return td->td_retval[0];
	} else {
		rtems_set_errno_and_return_minus_one(error);
	}
}
#endif /* __rtems__ */

#ifdef __rtems__
static int kern_bindat(struct thread *td, int dirfd, int fd,
    struct sockaddr *sa);

static
#endif /* __rtems__ */
int
sys_bind(struct thread *td, struct bind_args *uap)
{
	struct sockaddr *sa;
	int error;
#ifdef __rtems__
	struct getsockaddr_sockaddr gsa;
	sa = &gsa.header;
#endif /* __rtems__ */

	error = getsockaddr(&sa, uap->name, uap->namelen);
	if (error == 0) {
		error = kern_bindat(td, AT_FDCWD, uap->s, sa);
#ifndef __rtems__
		free(sa, M_SONAME);
#endif /* __rtems__ */
	}
	return (error);
}
#ifdef __rtems__
int
bind(int socket, const struct sockaddr *address, socklen_t address_len)
{
	struct thread *td = rtems_bsd_get_curthread_or_null();
	struct bind_args ua = {
		.s = socket,
		.name = address,
		.namelen = address_len
	};
	int error;

	if (td != NULL) {
		error = sys_bind(td, &ua);
	} else {
		error = ENOMEM;
	}

	return rtems_bsd_error_to_status_and_errno(error);
}
#endif /* __rtems__ */

int
kern_bindat(struct thread *td, int dirfd, int fd, struct sockaddr *sa)
{
	struct socket *so;
	struct file *fp;
	int error;

#ifdef CAPABILITY_MODE
	if (IN_CAPABILITY_MODE(td) && (dirfd == AT_FDCWD))
		return (ECAPMODE);
#endif

	AUDIT_ARG_FD(fd);
	AUDIT_ARG_SOCKADDR(td, dirfd, sa);
	error = getsock_cap(td, fd, &cap_bind_rights,
	    &fp, NULL, NULL);
	if (error != 0)
		return (error);
	so = fp->f_data;
#ifdef KTRACE
	if (KTRPOINT(td, KTR_STRUCT))
		ktrsockaddr(sa);
#endif
#ifdef MAC
	error = mac_socket_check_bind(td->td_ucred, so, sa);
	if (error == 0) {
#endif
		if (dirfd == AT_FDCWD)
			error = sobind(so, sa, td);
		else
			error = sobindat(dirfd, so, sa, td);
#ifdef MAC
	}
#endif
	fdrop(fp, td);
	return (error);
}

#ifndef __rtems__
static
int
sys_bindat(struct thread *td, struct bindat_args *uap)
{
	struct sockaddr *sa;
	int error;
#ifdef __rtems__
	struct getsockaddr_sockaddr gsa;
	sa = &gsa.header;
#endif /* __rtems__ */

	error = getsockaddr(&sa, uap->name, uap->namelen);
	if (error == 0) {
		error = kern_bindat(td, uap->fd, uap->s, sa);
#ifndef __rtems__
		free(sa, M_SONAME);
#endif /* __rtems__ */
	}
	return (error);
}
#endif /* __rtems__ */

#ifdef __rtems__
static
#endif /* __rtems__ */
int
sys_listen(struct thread *td, struct listen_args *uap)
{

	return (kern_listen(td, uap->s, uap->backlog));
}

int
kern_listen(struct thread *td, int s, int backlog)
{
	struct socket *so;
	struct file *fp;
	int error;

	AUDIT_ARG_FD(s);
	error = getsock_cap(td, s, &cap_listen_rights,
	    &fp, NULL, NULL);
	if (error == 0) {
		so = fp->f_data;
#ifdef MAC
		error = mac_socket_check_listen(td->td_ucred, so);
		if (error == 0)
#endif
			error = solisten(so, backlog, td);
		fdrop(fp, td);
	}
	return (error);
}
#ifdef __rtems__
int
listen(int socket, int backlog)
{
	struct thread *td = rtems_bsd_get_curthread_or_null();
	struct listen_args ua = {
		.s = socket,
		.backlog = backlog
	};
	int error;

	if (td != NULL) {
		error = sys_listen(td, &ua);
	} else {
		error = ENOMEM;
	}

	return rtems_bsd_error_to_status_and_errno(error);
}
#endif /* __rtems__ */

#ifdef __rtems__
static int kern_accept4(struct thread *td, int s, struct sockaddr **name,
    socklen_t *namelen, int flags, struct file **fp);
#endif /* __rtems__ */
/*
 * accept1()
 */
static int
accept1(td, s, uname, anamelen, flags)
	struct thread *td;
	int s;
	struct sockaddr *uname;
	socklen_t *anamelen;
	int flags;
{
	struct sockaddr *name;
	socklen_t namelen;
	struct file *fp;
	int error;

	if (uname == NULL)
		return (kern_accept4(td, s, NULL, NULL, flags, NULL));

	error = copyin(anamelen, &namelen, sizeof (namelen));
	if (error != 0)
		return (error);

	error = kern_accept4(td, s, &name, &namelen, flags, &fp);

	if (error != 0)
		return (error);

	if (error == 0 && uname != NULL) {
#ifdef COMPAT_OLDSOCK
		if (SV_PROC_FLAG(td->td_proc, SV_AOUT) &&
		    (flags & ACCEPT4_COMPAT) != 0)
			((struct osockaddr *)name)->sa_family =
			    name->sa_family;
#endif
		error = copyout(name, uname, namelen);
	}
	if (error == 0)
		error = copyout(&namelen, anamelen,
		    sizeof(namelen));
	if (error != 0)
		fdclose(td, fp, td->td_retval[0]);
#ifndef __rtems__
	fdrop(fp, td);
#endif /* __rtems__ */
	free(name, M_SONAME);
	return (error);
}
#ifdef __rtems__
int
accept(int socket, struct sockaddr *__restrict address,
    socklen_t *__restrict address_len)
{
	struct thread *td = rtems_bsd_get_curthread_or_null();
	int error;

	if (td != NULL) {
		error = accept1(td, socket, address, address_len,
		    ACCEPT4_INHERIT);
	} else {
		error = ENOMEM;
	}

	if (error == 0) {
		return td->td_retval[0];
	} else {
		rtems_set_errno_and_return_minus_one(error);
	}
}
#endif /* __rtems__ */

#ifndef __rtems__
int
kern_accept(struct thread *td, int s, struct sockaddr **name,
    socklen_t *namelen, struct file **fp)
{
	return (kern_accept4(td, s, name, namelen, ACCEPT4_INHERIT, fp));
}
#endif /* __rtems__ */

int
kern_accept4(struct thread *td, int s, struct sockaddr **name,
    socklen_t *namelen, int flags, struct file **fp)
{
	struct file *headfp, *nfp = NULL;
	struct sockaddr *sa = NULL;
	struct socket *head, *so;
#ifndef __rtems__
	struct filecaps fcaps;
#endif /* __rtems__ */
	u_int fflag;
	pid_t pgid;
	int error, fd, tmp;

	if (name != NULL)
		*name = NULL;

	AUDIT_ARG_FD(s);
	error = getsock_cap(td, s, &cap_accept_rights,
	    &headfp, &fflag, &fcaps);
	if (error != 0)
		return (error);
	head = headfp->f_data;
	if ((head->so_options & SO_ACCEPTCONN) == 0) {
		error = EINVAL;
		goto done;
	}
#ifdef MAC
	error = mac_socket_check_accept(td->td_ucred, head);
	if (error != 0)
		goto done;
#endif
	error = falloc_caps(td, &nfp, &fd,
	    (flags & SOCK_CLOEXEC) ? O_CLOEXEC : 0, &fcaps);
	if (error != 0)
		goto done;
	SOCK_LOCK(head);
	if (!SOLISTENING(head)) {
		SOCK_UNLOCK(head);
		error = EINVAL;
		goto noconnection;
	}

	error = solisten_dequeue(head, &so, flags);
	if (error != 0)
		goto noconnection;

	/* An extra reference on `nfp' has been held for us by falloc(). */
	td->td_retval[0] = fd;

	/* Connection has been removed from the listen queue. */
	KNOTE_UNLOCKED(&head->so_rdsel.si_note, 0);

	if (flags & ACCEPT4_INHERIT) {
		pgid = fgetown(&head->so_sigio);
		if (pgid != 0)
			fsetown(pgid, &so->so_sigio);
	} else {
		fflag &= ~(FNONBLOCK | FASYNC);
		if (flags & SOCK_NONBLOCK)
			fflag |= FNONBLOCK;
	}

	finit(nfp, fflag, DTYPE_SOCKET, so, &socketops);
	/* Sync socket nonblocking/async state with file flags */
	tmp = fflag & FNONBLOCK;
	(void) fo_ioctl(nfp, FIONBIO, &tmp, td->td_ucred, td);
	tmp = fflag & FASYNC;
	(void) fo_ioctl(nfp, FIOASYNC, &tmp, td->td_ucred, td);
	error = soaccept(so, &sa);
	if (error != 0)
		goto noconnection;
	if (sa == NULL) {
		if (name)
			*namelen = 0;
		goto done;
	}
	AUDIT_ARG_SOCKADDR(td, AT_FDCWD, sa);
	if (name) {
		/* check sa_len before it is destroyed */
		if (*namelen > sa->sa_len)
			*namelen = sa->sa_len;
#ifdef KTRACE
		if (KTRPOINT(td, KTR_STRUCT))
			ktrsockaddr(sa);
#endif
		*name = sa;
		sa = NULL;
	}
noconnection:
	free(sa, M_SONAME);

	/*
	 * close the new descriptor, assuming someone hasn't ripped it
	 * out from under us.
	 */
	if (error != 0)
		fdclose(td, nfp, fd);

	/*
	 * Release explicitly held references before returning.  We return
	 * a reference on nfp to the caller on success if they request it.
	 */
done:
	if (nfp == NULL)
		filecaps_free(&fcaps);
	if (fp != NULL) {
		if (error == 0) {
			*fp = nfp;
			nfp = NULL;
		} else
			*fp = NULL;
	}
#ifndef __rtems__
	if (nfp != NULL)
		fdrop(nfp, td);
#endif /* __rtems__ */
	fdrop(headfp, td);
	return (error);
}

#ifndef __rtems__
int
sys_accept(td, uap)
	struct thread *td;
	struct accept_args *uap;
{

	return (accept1(td, uap->s, uap->name, uap->anamelen, ACCEPT4_INHERIT));
}

int
sys_accept4(td, uap)
	struct thread *td;
	struct accept4_args *uap;
{

	if (uap->flags & ~(SOCK_CLOEXEC | SOCK_NONBLOCK))
		return (EINVAL);

	return (accept1(td, uap->s, uap->name, uap->anamelen, uap->flags));
}

#ifdef COMPAT_OLDSOCK
int
oaccept(td, uap)
	struct thread *td;
	struct accept_args *uap;
{

	return (accept1(td, uap->s, uap->name, uap->anamelen,
	    ACCEPT4_INHERIT | ACCEPT4_COMPAT));
}
#endif /* COMPAT_OLDSOCK */
#endif /* __rtems__ */

#ifdef __rtems__
static int kern_connectat(struct thread *td, int dirfd, int fd,
    struct sockaddr *sa);

static
#endif /* __rtems__ */
int
sys_connect(struct thread *td, struct connect_args *uap)
{
	struct sockaddr *sa;
	int error;
#ifdef __rtems__
	struct getsockaddr_sockaddr gsa;
	sa = &gsa.header;
#endif /* __rtems__ */

	error = getsockaddr(&sa, uap->name, uap->namelen);
	if (error == 0) {
		error = kern_connectat(td, AT_FDCWD, uap->s, sa);
#ifndef __rtems__
		free(sa, M_SONAME);
#endif /* __rtems__ */
	}
	return (error);
}
#ifdef __rtems__
int
connect(int socket, const struct sockaddr *address, socklen_t address_len)
{
	struct thread *td = rtems_bsd_get_curthread_or_null();
	struct connect_args ua = {
		.s = socket,
		.name = address,
		.namelen = address_len
	};
	int error;

	if (td != NULL) {
		error = sys_connect(td, &ua);
	} else {
		error = ENOMEM;
	}

	return rtems_bsd_error_to_status_and_errno(error);
}
#endif /* __rtems__ */

int
kern_connectat(struct thread *td, int dirfd, int fd, struct sockaddr *sa)
{
	struct socket *so;
	struct file *fp;
	int error, interrupted = 0;

#ifdef CAPABILITY_MODE
	if (IN_CAPABILITY_MODE(td) && (dirfd == AT_FDCWD))
		return (ECAPMODE);
#endif

	AUDIT_ARG_FD(fd);
	AUDIT_ARG_SOCKADDR(td, dirfd, sa);
	error = getsock_cap(td, fd, &cap_connect_rights,
	    &fp, NULL, NULL);
	if (error != 0)
		return (error);
	so = fp->f_data;
	if (so->so_state & SS_ISCONNECTING) {
		error = EALREADY;
		goto done1;
	}
#ifdef KTRACE
	if (KTRPOINT(td, KTR_STRUCT))
		ktrsockaddr(sa);
#endif
#ifdef MAC
	error = mac_socket_check_connect(td->td_ucred, so, sa);
	if (error != 0)
		goto bad;
#endif
	if (dirfd == AT_FDCWD)
		error = soconnect(so, sa, td);
	else
		error = soconnectat(dirfd, so, sa, td);
	if (error != 0)
		goto bad;
	if ((so->so_state & SS_NBIO) && (so->so_state & SS_ISCONNECTING)) {
		error = EINPROGRESS;
		goto done1;
	}
	SOCK_LOCK(so);
	while ((so->so_state & SS_ISCONNECTING) && so->so_error == 0) {
		error = msleep(&so->so_timeo, &so->so_lock, PSOCK | PCATCH,
		    "connec", 0);
		if (error != 0) {
			if (error == EINTR || error == ERESTART)
				interrupted = 1;
			break;
		}
	}
	if (error == 0) {
		error = so->so_error;
		so->so_error = 0;
	}
	SOCK_UNLOCK(so);
bad:
	if (!interrupted)
		so->so_state &= ~SS_ISCONNECTING;
	if (error == ERESTART)
		error = EINTR;
done1:
	fdrop(fp, td);
	return (error);
}

#ifndef __rtems__
int
sys_connectat(struct thread *td, struct connectat_args *uap)
{
	struct sockaddr *sa;
	int error;
#ifdef __rtems__
	struct getsockaddr_sockaddr gsa;
	sa = &gsa.header;
#endif /* __rtems__ */

	error = getsockaddr(&sa, uap->name, uap->namelen);
	if (error == 0) {
		error = kern_connectat(td, uap->fd, uap->s, sa);
#ifndef __rtems__
		free(sa, M_SONAME);
#endif /* __rtems__ */
	}
	return (error);
}
#endif /* __rtems__ */

int
kern_socketpair(struct thread *td, int domain, int type, int protocol,
    int *rsv)
{
	struct file *fp1, *fp2;
	struct socket *so1, *so2;
	int fd, error, oflag, fflag;

	AUDIT_ARG_SOCKET(domain, type, protocol);

	oflag = 0;
	fflag = 0;
	if ((type & SOCK_CLOEXEC) != 0) {
		type &= ~SOCK_CLOEXEC;
		oflag |= O_CLOEXEC;
	}
	if ((type & SOCK_NONBLOCK) != 0) {
		type &= ~SOCK_NONBLOCK;
		fflag |= FNONBLOCK;
	}
#ifdef MAC
	/* We might want to have a separate check for socket pairs. */
	error = mac_socket_check_create(td->td_ucred, domain, type,
	    protocol);
	if (error != 0)
		return (error);
#endif
	error = socreate(domain, &so1, type, protocol, td->td_ucred, td);
	if (error != 0)
		return (error);
	error = socreate(domain, &so2, type, protocol, td->td_ucred, td);
	if (error != 0)
		goto free1;
	/* On success extra reference to `fp1' and 'fp2' is set by falloc. */
	error = falloc(td, &fp1, &fd, oflag);
	if (error != 0)
		goto free2;
	rsv[0] = fd;
	fp1->f_data = so1;	/* so1 already has ref count */
	error = falloc(td, &fp2, &fd, oflag);
	if (error != 0)
		goto free3;
	fp2->f_data = so2;	/* so2 already has ref count */
	rsv[1] = fd;
	error = soconnect2(so1, so2);
	if (error != 0)
		goto free4;
	if (type == SOCK_DGRAM) {
		/*
		 * Datagram socket connection is asymmetric.
		 */
		 error = soconnect2(so2, so1);
		 if (error != 0)
			goto free4;
	} else if (so1->so_proto->pr_flags & PR_CONNREQUIRED) {
		struct unpcb *unp, *unp2;
		unp = sotounpcb(so1);
		unp2 = sotounpcb(so2);
		/* 
		 * No need to lock the unps, because the sockets are brand-new.
		 * No other threads can be using them yet
		 */
		unp_copy_peercred(td, unp, unp2, unp);
	}
	finit(fp1, FREAD | FWRITE | fflag, DTYPE_SOCKET, fp1->f_data,
	    &socketops);
	finit(fp2, FREAD | FWRITE | fflag, DTYPE_SOCKET, fp2->f_data,
	    &socketops);
	if ((fflag & FNONBLOCK) != 0) {
		(void) fo_ioctl(fp1, FIONBIO, &fflag, td->td_ucred, td);
		(void) fo_ioctl(fp2, FIONBIO, &fflag, td->td_ucred, td);
	}
#ifndef __rtems__
	fdrop(fp1, td);
	fdrop(fp2, td);
#endif /* __rtems__ */
	return (0);
free4:
	fdclose(td, fp2, rsv[1]);
#ifndef __rtems__
	fdrop(fp2, td);
#endif /* __rtems__ */
free3:
	fdclose(td, fp1, rsv[0]);
#ifndef __rtems__
	fdrop(fp1, td);
#endif /* __rtems__ */
free2:
	if (so2 != NULL)
		(void)soclose(so2);
free1:
	if (so1 != NULL)
		(void)soclose(so1);
	return (error);
}

#ifdef __rtems__
static
#endif /* __rtems__ */
int
sys_socketpair(struct thread *td, struct socketpair_args *uap)
{
#ifndef __rtems__
	int error, sv[2];
#else /* __rtems__ */
	int error;
	int *sv = uap->rsv;
#endif /* __rtems__ */

	error = kern_socketpair(td, uap->domain, uap->type,
	    uap->protocol, sv);
	if (error != 0)
		return (error);
#ifndef __rtems__
	error = copyout(sv, uap->rsv, 2 * sizeof(int));
	if (error != 0) {
		(void)kern_close(td, sv[0]);
		(void)kern_close(td, sv[1]);
	}
#endif /* __rtems__ */
	return (error);
}
#ifdef __rtems__
int
socketpair(int domain, int type, int protocol, int *socket_vector)
{
	struct thread *td = rtems_bsd_get_curthread_or_null();
	struct socketpair_args ua = {
		.domain = domain,
		.type = type,
		.protocol = protocol,
		.rsv = socket_vector
	};
	int error;

	if (td != NULL) {
		error = sys_socketpair(td, &ua);
	} else {
		error = ENOMEM;
	}

	return rtems_bsd_error_to_status_and_errno(error);
}
#endif /* __rtems__ */

#ifdef __rtems__
static int
kern_sendit( struct thread *td, int s, struct msghdr *mp, int flags,
    struct mbuf *control, enum uio_seg segflg);
#endif /* __rtems__ */
static int
sendit(struct thread *td, int s, struct msghdr *mp, int flags)
{
	struct mbuf *control;
	struct sockaddr *to;
	int error;
#ifdef __rtems__
	struct getsockaddr_sockaddr gto;
	to = &gto.header;
#endif /* __rtems__ */

#ifdef CAPABILITY_MODE
	if (IN_CAPABILITY_MODE(td) && (mp->msg_name != NULL))
		return (ECAPMODE);
#endif

	if (mp->msg_name != NULL) {
		error = getsockaddr(&to, mp->msg_name, mp->msg_namelen);
		if (error != 0) {
			to = NULL;
			goto bad;
		}
		mp->msg_name = to;
	} else {
		to = NULL;
	}

	if (mp->msg_control) {
		if (mp->msg_controllen < sizeof(struct cmsghdr)
#ifdef COMPAT_OLDSOCK
		    && (mp->msg_flags != MSG_COMPAT ||
		    !SV_PROC_FLAG(td->td_proc, SV_AOUT))
#endif
		) {
			error = EINVAL;
			goto bad;
		}
		error = sockargs(&control, mp->msg_control,
		    mp->msg_controllen, MT_CONTROL);
		if (error != 0)
			goto bad;
#ifdef COMPAT_OLDSOCK
		if (mp->msg_flags == MSG_COMPAT &&
		    SV_PROC_FLAG(td->td_proc, SV_AOUT)) {
			struct cmsghdr *cm;

			M_PREPEND(control, sizeof(*cm), M_WAITOK);
			cm = mtod(control, struct cmsghdr *);
			cm->cmsg_len = control->m_len;
			cm->cmsg_level = SOL_SOCKET;
			cm->cmsg_type = SCM_RIGHTS;
		}
#endif
	} else {
		control = NULL;
	}

	error = kern_sendit(td, s, mp, flags, control, UIO_USERSPACE);

bad:
#ifndef __rtems__
	free(to, M_SONAME);
#endif /* __rtems__ */
	return (error);
}

int
kern_sendit(struct thread *td, int s, struct msghdr *mp, int flags,
    struct mbuf *control, enum uio_seg segflg)
{
	struct file *fp;
	struct uio auio;
	struct iovec *iov;
	struct socket *so;
#ifndef __rtems__
	cap_rights_t *rights;
#endif /* __rtems__ */
#ifdef KTRACE
	struct uio *ktruio = NULL;
#endif
	ssize_t len;
	int i, error;

	AUDIT_ARG_FD(s);
#ifndef __rtems__
	rights = &cap_send_rights;
	cap_rights_init(&rights, CAP_SEND);
	if (mp->msg_name != NULL) {
		AUDIT_ARG_SOCKADDR(td, AT_FDCWD, mp->msg_name);
		rights = &cap_send_connect_rights;
	}
#endif /* __rtems__ */
	error = getsock_cap(td, s, rights, &fp, NULL, NULL);
	if (error != 0) {
		m_freem(control);
		return (error);
	}
	so = (struct socket *)fp->f_data;

#ifdef KTRACE
	if (mp->msg_name != NULL && KTRPOINT(td, KTR_STRUCT))
		ktrsockaddr(mp->msg_name);
#endif
#ifdef MAC
	if (mp->msg_name != NULL) {
		error = mac_socket_check_connect(td->td_ucred, so,
		    mp->msg_name);
		if (error != 0) {
			m_freem(control);
			goto bad;
		}
	}
	error = mac_socket_check_send(td->td_ucred, so);
	if (error != 0) {
		m_freem(control);
		goto bad;
	}
#endif

	auio.uio_iov = mp->msg_iov;
	auio.uio_iovcnt = mp->msg_iovlen;
	auio.uio_segflg = segflg;
	auio.uio_rw = UIO_WRITE;
	auio.uio_td = td;
	auio.uio_offset = 0;			/* XXX */
	auio.uio_resid = 0;
	iov = mp->msg_iov;
	for (i = 0; i < mp->msg_iovlen; i++, iov++) {
		if ((auio.uio_resid += iov->iov_len) < 0) {
			error = EINVAL;
			m_freem(control);
			goto bad;
		}
	}
#ifdef KTRACE
	if (KTRPOINT(td, KTR_GENIO))
		ktruio = cloneuio(&auio);
#endif
	len = auio.uio_resid;
	error = sosend(so, mp->msg_name, &auio, 0, control, flags, td);
	if (error != 0) {
		if (auio.uio_resid != len && (error == ERESTART ||
		    error == EINTR || error == EWOULDBLOCK))
			error = 0;
		/* Generation of SIGPIPE can be controlled per socket */
		if (error == EPIPE && !(so->so_options & SO_NOSIGPIPE) &&
		    !(flags & MSG_NOSIGNAL)) {
#ifndef __rtems__
			PROC_LOCK(td->td_proc);
			tdsignal(td, SIGPIPE);
			PROC_UNLOCK(td->td_proc);
#else /* __rtems__ */
		/* FIXME: Determine if we really want to use signals */
#endif /* __rtems__ */
		}
	}
	if (error == 0)
		td->td_retval[0] = len - auio.uio_resid;
#ifdef KTRACE
	if (ktruio != NULL) {
		ktruio->uio_resid = td->td_retval[0];
		ktrgenio(s, UIO_WRITE, ktruio, error);
	}
#endif
bad:
	fdrop(fp, td);
	return (error);
}

#ifdef __rtems__
static
#endif /* __rtems__ */
int
sys_sendto(struct thread *td, struct sendto_args *uap)
{
	struct msghdr msg;
	struct iovec aiov;

#ifndef __rtems__
	msg.msg_name = uap->to;
#else /* __rtems__ */
	msg.msg_name = __DECONST(void *, uap->to);
#endif /* __rtems__ */
	msg.msg_namelen = uap->tolen;
	msg.msg_iov = &aiov;
	msg.msg_iovlen = 1;
	msg.msg_control = 0;
#ifdef COMPAT_OLDSOCK
	if (SV_PROC_FLAG(td->td_proc, SV_AOUT))
		msg.msg_flags = 0;
#endif
	aiov.iov_base = uap->buf;
	aiov.iov_len = uap->len;
	return (sendit(td, uap->s, &msg, uap->flags));
}
#ifdef __rtems__
ssize_t
sendto(int socket, const void *message, size_t length, int flags,
    const struct sockaddr *dest_addr, socklen_t dest_len)
{
	struct thread *td = rtems_bsd_get_curthread_or_null();
	struct sendto_args ua = {
		.s = socket,
		.buf = (caddr_t) message,
		.len = length,
		.flags = flags,
		.to = dest_addr,
		.tolen = dest_len
	};
	int error;

	if (td != NULL) {
		error = sys_sendto(td, &ua);
	} else {
		error = ENOMEM;
	}

	if (error == 0) {
		return td->td_retval[0];
	} else {
		rtems_set_errno_and_return_minus_one(error);
	}
}

int
rtems_bsd_sendto(int socket, struct mbuf *m, int flags,
    const struct sockaddr *dest_addr)
{
	struct thread *td = rtems_bsd_get_curthread_or_null();
	struct file *fp;
	struct socket *so;
	int error;

	error = getsock_cap(td, socket, CAP_WRITE, &fp, NULL, NULL);
	if (error)
		return (error);
	so = (struct socket *)fp->f_data;

	if (td != NULL) {
		error = sosend(so, __DECONST(struct sockaddr *, dest_addr),
		    NULL, m, NULL, flags, td);
	} else {
		error = ENOMEM;
	}

	return (error);
}
#endif /* __rtems__ */

#ifndef __rtems__
#ifdef COMPAT_OLDSOCK
int
osend(struct thread *td, struct osend_args *uap)
{
	struct msghdr msg;
	struct iovec aiov;

	msg.msg_name = 0;
	msg.msg_namelen = 0;
	msg.msg_iov = &aiov;
	msg.msg_iovlen = 1;
	aiov.iov_base = uap->buf;
	aiov.iov_len = uap->len;
	msg.msg_control = 0;
	msg.msg_flags = 0;
	return (sendit(td, uap->s, &msg, uap->flags));
}

int
osendmsg(struct thread *td, struct osendmsg_args *uap)
{
	struct msghdr msg;
	struct iovec *iov;
	int error;

	error = copyin(uap->msg, &msg, sizeof (struct omsghdr));
	if (error != 0)
		return (error);
	error = copyiniov(msg.msg_iov, msg.msg_iovlen, &iov, EMSGSIZE);
	if (error != 0)
		return (error);
	msg.msg_iov = iov;
	msg.msg_flags = MSG_COMPAT;
	error = sendit(td, uap->s, &msg, uap->flags);
	free(iov, M_IOV);
	return (error);
}
#endif
#endif /* __rtems__ */

#ifdef __rtems__
static
#endif /* __rtems__ */
int
sys_sendmsg(struct thread *td, struct sendmsg_args *uap)
{
	struct msghdr msg;
	struct iovec *iov;
	int error;

	error = copyin(uap->msg, &msg, sizeof (msg));
	if (error != 0)
		return (error);
	error = copyiniov(msg.msg_iov, msg.msg_iovlen, &iov, EMSGSIZE);
	if (error != 0)
		return (error);
	msg.msg_iov = iov;
#ifdef COMPAT_OLDSOCK
	if (SV_PROC_FLAG(td->td_proc, SV_AOUT))
		msg.msg_flags = 0;
#endif
	error = sendit(td, uap->s, &msg, uap->flags);
	free(iov, M_IOV);
	return (error);
}
#ifdef __rtems__
ssize_t
sendmsg(int socket, const struct msghdr *message, int flags)
{
	struct thread *td = rtems_bsd_get_curthread_or_null();
	struct sendmsg_args ua = {
		.s = socket,
		.msg = message,
		.flags = flags
	};
	int error;

	if (td != NULL) {
		error = sys_sendmsg(td, &ua);
	} else {
		error = ENOMEM;
	}

	if (error == 0) {
		return td->td_retval[0];
	} else {
		rtems_set_errno_and_return_minus_one(error);
	}
}
#endif /* __rtems__ */

#ifdef __rtems__
static
#endif /* __rtems__ */
int
kern_recvit(struct thread *td, int s, struct msghdr *mp, enum uio_seg fromseg,
    struct mbuf **controlp)
{
	struct uio auio;
	struct iovec *iov;
	struct mbuf *control, *m;
	caddr_t ctlbuf;
	struct file *fp;
	struct socket *so;
	struct sockaddr *fromsa = NULL;
#ifdef KTRACE
	struct uio *ktruio = NULL;
#endif
	ssize_t len;
	int error, i;

	if (controlp != NULL)
		*controlp = NULL;

	AUDIT_ARG_FD(s);
	error = getsock_cap(td, s, &cap_recv_rights,
	    &fp, NULL, NULL);
	if (error != 0)
		return (error);
	so = fp->f_data;

#ifdef MAC
	error = mac_socket_check_receive(td->td_ucred, so);
	if (error != 0) {
		fdrop(fp, td);
		return (error);
	}
#endif

	auio.uio_iov = mp->msg_iov;
	auio.uio_iovcnt = mp->msg_iovlen;
	auio.uio_segflg = UIO_USERSPACE;
	auio.uio_rw = UIO_READ;
	auio.uio_td = td;
	auio.uio_offset = 0;			/* XXX */
	auio.uio_resid = 0;
	iov = mp->msg_iov;
	for (i = 0; i < mp->msg_iovlen; i++, iov++) {
		if ((auio.uio_resid += iov->iov_len) < 0) {
			fdrop(fp, td);
			return (EINVAL);
		}
	}
#ifdef KTRACE
	if (KTRPOINT(td, KTR_GENIO))
		ktruio = cloneuio(&auio);
#endif
	control = NULL;
	len = auio.uio_resid;
	error = soreceive(so, &fromsa, &auio, NULL,
	    (mp->msg_control || controlp) ? &control : NULL,
	    &mp->msg_flags);
	if (error != 0) {
		if (auio.uio_resid != len && (error == ERESTART ||
		    error == EINTR || error == EWOULDBLOCK))
			error = 0;
	}
	if (fromsa != NULL)
		AUDIT_ARG_SOCKADDR(td, AT_FDCWD, fromsa);
#ifdef KTRACE
	if (ktruio != NULL) {
		ktruio->uio_resid = len - auio.uio_resid;
		ktrgenio(s, UIO_READ, ktruio, error);
	}
#endif
	if (error != 0)
		goto out;
	td->td_retval[0] = len - auio.uio_resid;
	if (mp->msg_name) {
		len = mp->msg_namelen;
		if (len <= 0 || fromsa == NULL)
			len = 0;
		else {
			/* save sa_len before it is destroyed by MSG_COMPAT */
			len = MIN(len, fromsa->sa_len);
#ifdef COMPAT_OLDSOCK
			if ((mp->msg_flags & MSG_COMPAT) != 0 &&
			    SV_PROC_FLAG(td->td_proc, SV_AOUT))
				((struct osockaddr *)fromsa)->sa_family =
				    fromsa->sa_family;
#endif
			if (fromseg == UIO_USERSPACE) {
				error = copyout(fromsa, mp->msg_name,
				    (unsigned)len);
				if (error != 0)
					goto out;
			} else
				bcopy(fromsa, mp->msg_name, len);
		}
		mp->msg_namelen = len;
	}
	if (mp->msg_control && controlp == NULL) {
#ifdef COMPAT_OLDSOCK
		/*
		 * We assume that old recvmsg calls won't receive access
		 * rights and other control info, esp. as control info
		 * is always optional and those options didn't exist in 4.3.
		 * If we receive rights, trim the cmsghdr; anything else
		 * is tossed.
		 */
		if (control && (mp->msg_flags & MSG_COMPAT) != 0 &&
		    SV_PROC_FLAG(td->td_proc, SV_AOUT)) {
			if (mtod(control, struct cmsghdr *)->cmsg_level !=
			    SOL_SOCKET ||
			    mtod(control, struct cmsghdr *)->cmsg_type !=
			    SCM_RIGHTS) {
				mp->msg_controllen = 0;
				goto out;
			}
			control->m_len -= sizeof (struct cmsghdr);
			control->m_data += sizeof (struct cmsghdr);
		}
#endif
		ctlbuf = mp->msg_control;
		len = mp->msg_controllen;
		mp->msg_controllen = 0;
		for (m = control; m != NULL && len >= m->m_len; m = m->m_next) {
			if ((error = copyout(mtod(m, caddr_t), ctlbuf,
			    m->m_len)) != 0)
				goto out;

			ctlbuf += m->m_len;
			len -= m->m_len;
			mp->msg_controllen += m->m_len;
		}
		if (m != NULL) {
			mp->msg_flags |= MSG_CTRUNC;
			m_dispose_extcontrolm(m);
		}
	}
out:
	fdrop(fp, td);
#ifdef KTRACE
	if (fromsa && KTRPOINT(td, KTR_STRUCT))
		ktrsockaddr(fromsa);
#endif
	free(fromsa, M_SONAME);

	if (error == 0 && controlp != NULL)
		*controlp = control;
	else if (control != NULL) {
		if (error != 0)
			m_dispose_extcontrolm(control);
		m_freem(control);
	}

	return (error);
}

static int
recvit(struct thread *td, int s, struct msghdr *mp, void *namelenp)
{
	int error;

	error = kern_recvit(td, s, mp, UIO_USERSPACE, NULL);
	if (error != 0)
		return (error);
	if (namelenp != NULL) {
		error = copyout(&mp->msg_namelen, namelenp, sizeof (socklen_t));
#ifdef COMPAT_OLDSOCK
		if ((mp->msg_flags & MSG_COMPAT) != 0 &&
		    SV_PROC_FLAG(td->td_proc, SV_AOUT))
			error = 0;	/* old recvfrom didn't check */
#endif
	}
	return (error);
}

#ifdef __rtems__
static
#endif /* __rtems__ */
int
sys_recvfrom(struct thread *td, struct recvfrom_args *uap)
{
	struct msghdr msg;
	struct iovec aiov;
	int error;

	if (uap->fromlenaddr) {
		error = copyin(uap->fromlenaddr,
		    &msg.msg_namelen, sizeof (msg.msg_namelen));
		if (error != 0)
			goto done2;
	} else {
		msg.msg_namelen = 0;
	}
	msg.msg_name = uap->from;
	msg.msg_iov = &aiov;
	msg.msg_iovlen = 1;
	aiov.iov_base = uap->buf;
	aiov.iov_len = uap->len;
	msg.msg_control = 0;
	msg.msg_flags = uap->flags;
	error = recvit(td, uap->s, &msg, uap->fromlenaddr);
done2:
	return (error);
}
#ifdef __rtems__
ssize_t
recvfrom(int socket, void *__restrict buffer, size_t length, int flags,
    struct sockaddr *__restrict address, socklen_t *__restrict address_len)
{
	struct thread *td = rtems_bsd_get_curthread_or_null();
	struct recvfrom_args ua = {
		.s = socket,
		.buf = buffer,
		.len = length,
		.flags = flags,
		.from = address,
		.fromlenaddr = address_len
	};
	int error;

	if (td != NULL) {
		error = sys_recvfrom(td, &ua);
	} else {
		error = ENOMEM;
	}

	if (error == 0) {
		return td->td_retval[0];
	} else {
		rtems_set_errno_and_return_minus_one(error);
	}
}
#endif /* __rtems__ */

#ifndef __rtems__
#ifdef COMPAT_OLDSOCK
int
orecvfrom(struct thread *td, struct recvfrom_args *uap)
{

	uap->flags |= MSG_COMPAT;
	return (sys_recvfrom(td, uap));
}
#endif

#ifdef COMPAT_OLDSOCK
int
orecv(struct thread *td, struct orecv_args *uap)
{
	struct msghdr msg;
	struct iovec aiov;

	msg.msg_name = 0;
	msg.msg_namelen = 0;
	msg.msg_iov = &aiov;
	msg.msg_iovlen = 1;
	aiov.iov_base = uap->buf;
	aiov.iov_len = uap->len;
	msg.msg_control = 0;
	msg.msg_flags = uap->flags;
	return (recvit(td, uap->s, &msg, NULL));
}

/*
 * Old recvmsg.  This code takes advantage of the fact that the old msghdr
 * overlays the new one, missing only the flags, and with the (old) access
 * rights where the control fields are now.
 */
int
orecvmsg(struct thread *td, struct orecvmsg_args *uap)
{
	struct msghdr msg;
	struct iovec *iov;
	int error;

	error = copyin(uap->msg, &msg, sizeof (struct omsghdr));
	if (error != 0)
		return (error);
	error = copyiniov(msg.msg_iov, msg.msg_iovlen, &iov, EMSGSIZE);
	if (error != 0)
		return (error);
	msg.msg_flags = uap->flags | MSG_COMPAT;
	msg.msg_iov = iov;
	error = recvit(td, uap->s, &msg, &uap->msg->msg_namelen);
	if (msg.msg_controllen && error == 0)
		error = copyout(&msg.msg_controllen,
		    &uap->msg->msg_accrightslen, sizeof (int));
	free(iov, M_IOV);
	return (error);
}
#endif
#endif /* __rtems__ */

#ifdef __rtems__
static
#endif /* __rtems__ */
int
sys_recvmsg(struct thread *td, struct recvmsg_args *uap)
{
	struct msghdr msg;
	struct iovec *uiov, *iov;
	int error;

	error = copyin(uap->msg, &msg, sizeof (msg));
	if (error != 0)
		return (error);
	error = copyiniov(msg.msg_iov, msg.msg_iovlen, &iov, EMSGSIZE);
	if (error != 0)
		return (error);
	msg.msg_flags = uap->flags;
#ifdef COMPAT_OLDSOCK
	if (SV_PROC_FLAG(td->td_proc, SV_AOUT))
		msg.msg_flags &= ~MSG_COMPAT;
#endif
	uiov = msg.msg_iov;
	msg.msg_iov = iov;
	error = recvit(td, uap->s, &msg, NULL);
	if (error == 0) {
		msg.msg_iov = uiov;
		error = copyout(&msg, uap->msg, sizeof(msg));
	}
	free(iov, M_IOV);
	return (error);
}
#ifdef __rtems__
ssize_t
recvmsg(int socket, struct msghdr *message, int flags)
{
	struct thread *td = rtems_bsd_get_curthread_or_null();
	struct recvmsg_args ua = {
		.s = socket,
		.msg = message,
		.flags = flags
	};
	int error;

	if (td != NULL) {
		error = sys_recvmsg(td, &ua);
	} else {
		error = ENOMEM;
	}

	if (error == 0) {
		return td->td_retval[0];
	} else {
		rtems_set_errno_and_return_minus_one(error);
	}
}
#endif /* __rtems__ */

#ifdef __rtems__
static
#endif /* __rtems__ */
int
sys_shutdown(struct thread *td, struct shutdown_args *uap)
{

	return (kern_shutdown(td, uap->s, uap->how));
}

int
kern_shutdown(struct thread *td, int s, int how)
{
	struct socket *so;
	struct file *fp;
	int error;

	AUDIT_ARG_FD(s);
	error = getsock_cap(td, s, &cap_shutdown_rights,
	    &fp, NULL, NULL);
	if (error == 0) {
		so = fp->f_data;
		error = soshutdown(so, how);
#ifndef __rtems__
		/*
		 * Previous versions did not return ENOTCONN, but 0 in
		 * case the socket was not connected. Some important
		 * programs like syslogd up to r279016, 2015-02-19,
		 * still depend on this behavior.
		 */
		if (error == ENOTCONN &&
		    td->td_proc->p_osrel < P_OSREL_SHUTDOWN_ENOTCONN)
			error = 0;
#endif /* __rtems__ */
		fdrop(fp, td);
	}
	return (error);
}
#ifdef __rtems__
int
shutdown(int socket, int how)
{
	struct shutdown_args ua = {
		.s = socket,
		.how = how
	};
	int error = sys_shutdown(NULL, &ua);

	return rtems_bsd_error_to_status_and_errno(error);
}
#endif /* __rtems__ */

#ifdef __rtems__
static
#endif /* __rtems__ */
int
sys_setsockopt(struct thread *td, struct setsockopt_args *uap)
{

	return (kern_setsockopt(td, uap->s, uap->level, uap->name,
	    uap->val, UIO_USERSPACE, uap->valsize));
}
#ifdef __rtems__
int
setsockopt(int socket, int level, int option_name, const void *option_value,
    socklen_t option_len)
{
	struct thread *td = rtems_bsd_get_curthread_or_null();
	struct setsockopt_args ua = {
		.s = socket,
		.level = level,
		.name = option_name,
		.val = __DECONST(void *, option_value),
		.valsize = option_len
	};
	int error;

	if (td != NULL) {
		error = sys_setsockopt(td, &ua);
	} else {
		error = ENOMEM;
	}

	return rtems_bsd_error_to_status_and_errno(error);
}
#endif /* __rtems__ */

int
#ifndef __rtems__
kern_setsockopt(struct thread *td, int s, int level, int name, void *val,
    enum uio_seg valseg, socklen_t valsize)
#else /* __rtems__ */
kern_setsockopt(struct thread *td, int s, int level, int name, const void *val,
    enum uio_seg valseg, socklen_t valsize)
#endif /* __rtems__ */
{
	struct socket *so;
	struct file *fp;
	struct sockopt sopt;
	int error;

	if (val == NULL && valsize != 0)
		return (EFAULT);
	if ((int)valsize < 0)
		return (EINVAL);

	sopt.sopt_dir = SOPT_SET;
	sopt.sopt_level = level;
	sopt.sopt_name = name;
#ifndef __rtems__
	sopt.sopt_val = val;
#else /* __rtems__ */
	sopt.sopt_val = __DECONST(void *, val);
#endif /* __rtems__ */
	sopt.sopt_valsize = valsize;
	switch (valseg) {
	case UIO_USERSPACE:
		sopt.sopt_td = td;
		break;
	case UIO_SYSSPACE:
		sopt.sopt_td = NULL;
		break;
	default:
		panic("kern_setsockopt called with bad valseg");
	}

	AUDIT_ARG_FD(s);
	error = getsock_cap(td, s, &cap_setsockopt_rights,
	    &fp, NULL, NULL);
	if (error == 0) {
		so = fp->f_data;
		error = sosetopt(so, &sopt);
		fdrop(fp, td);
	}
	return(error);
}

#ifdef __rtems__
static int kern_getsockopt( struct thread *td, int s, int level, int name,
    void *val, enum uio_seg valseg, socklen_t *valsize);

static
#endif /* __rtems__ */
int
sys_getsockopt(struct thread *td, struct getsockopt_args *uap)
{
	socklen_t valsize;
	int error;

	if (uap->val) {
		error = copyin(uap->avalsize, &valsize, sizeof (valsize));
		if (error != 0)
			return (error);
	}

	error = kern_getsockopt(td, uap->s, uap->level, uap->name,
	    uap->val, UIO_USERSPACE, &valsize);

	if (error == 0)
		error = copyout(&valsize, uap->avalsize, sizeof (valsize));
	return (error);
}
#ifdef __rtems__
int
getsockopt(int socket, int level, int option_name, void *__restrict
    option_value, socklen_t *__restrict option_len)
{
	struct thread *td = rtems_bsd_get_curthread_or_null();
	struct getsockopt_args ua = {
		.s = socket,
		.level = level,
		.name = option_name,
		.val = (caddr_t) option_value,
		.avalsize = option_len
	};
	int error;

	if (td != NULL) {
		error = sys_getsockopt(td, &ua);
	} else {
		error = ENOMEM;
	}

	return rtems_bsd_error_to_status_and_errno(error);
}
#endif /* __rtems__ */

/*
 * Kernel version of getsockopt.
 * optval can be a userland or userspace. optlen is always a kernel pointer.
 */
int
kern_getsockopt(struct thread *td, int s, int level, int name, void *val,
    enum uio_seg valseg, socklen_t *valsize)
{
	struct socket *so;
	struct file *fp;
	struct sockopt sopt;
	int error;

	if (val == NULL)
		*valsize = 0;
	if ((int)*valsize < 0)
		return (EINVAL);

	sopt.sopt_dir = SOPT_GET;
	sopt.sopt_level = level;
	sopt.sopt_name = name;
	sopt.sopt_val = val;
	sopt.sopt_valsize = (size_t)*valsize; /* checked non-negative above */
	switch (valseg) {
	case UIO_USERSPACE:
		sopt.sopt_td = td;
		break;
	case UIO_SYSSPACE:
		sopt.sopt_td = NULL;
		break;
	default:
		panic("kern_getsockopt called with bad valseg");
	}

	AUDIT_ARG_FD(s);
	error = getsock_cap(td, s, &cap_getsockopt_rights,
	    &fp, NULL, NULL);
	if (error == 0) {
		so = fp->f_data;
		error = sogetopt(so, &sopt);
		*valsize = sopt.sopt_valsize;
		fdrop(fp, td);
	}
	return (error);
}

/*
 * getsockname1() - Get socket name.
 */
static int
getsockname1(struct thread *td, struct getsockname_args *uap, int compat)
{
	struct sockaddr *sa;
	socklen_t len;
	int error;

	error = copyin(uap->alen, &len, sizeof(len));
	if (error != 0)
		return (error);

	error = kern_getsockname(td, uap->fdes, &sa, &len);
	if (error != 0)
		return (error);

	if (len != 0) {
#ifdef COMPAT_OLDSOCK
		if (compat && SV_PROC_FLAG(td->td_proc, SV_AOUT))
			((struct osockaddr *)sa)->sa_family = sa->sa_family;
#endif
		error = copyout(sa, uap->asa, (u_int)len);
	}
	free(sa, M_SONAME);
	if (error == 0)
		error = copyout(&len, uap->alen, sizeof(len));
	return (error);
}
#ifdef __rtems__
int
getsockname(int socket, struct sockaddr *__restrict address,
    socklen_t *__restrict address_len)
{
	struct thread *td = rtems_bsd_get_curthread_or_null();
	struct getsockname_args ua = {
		.fdes = socket,
		.asa = address,
		.alen = address_len
	};
	int error;

	if (td != NULL) {
		error = getsockname1(td, &ua, 0);
	} else {
		error = ENOMEM;
	}

	return rtems_bsd_error_to_status_and_errno(error);
}
#endif /* __rtems__ */

int
kern_getsockname(struct thread *td, int fd, struct sockaddr **sa,
    socklen_t *alen)
{
	struct socket *so;
	struct file *fp;
	socklen_t len;
	int error;

	AUDIT_ARG_FD(fd);
	error = getsock_cap(td, fd, &cap_getsockname_rights,
	    &fp, NULL, NULL);
	if (error != 0)
		return (error);
	so = fp->f_data;
	*sa = NULL;
	CURVNET_SET(so->so_vnet);
	error = (*so->so_proto->pr_usrreqs->pru_sockaddr)(so, sa);
	CURVNET_RESTORE();
	if (error != 0)
		goto bad;
	if (*sa == NULL)
		len = 0;
	else
		len = MIN(*alen, (*sa)->sa_len);
	*alen = len;
#ifdef KTRACE
	if (KTRPOINT(td, KTR_STRUCT))
		ktrsockaddr(*sa);
#endif
bad:
	fdrop(fp, td);
	if (error != 0 && *sa != NULL) {
		free(*sa, M_SONAME);
		*sa = NULL;
	}
	return (error);
}

#ifndef __rtems__
int
sys_getsockname(struct thread *td, struct getsockname_args *uap)
{

	return (getsockname1(td, uap, 0));
}

#ifdef COMPAT_OLDSOCK
int
ogetsockname(struct thread *td, struct getsockname_args *uap)
{

	return (getsockname1(td, uap, 1));
}
#endif /* COMPAT_OLDSOCK */
#endif /* __rtems__ */

#ifdef __rtems__
static int
kern_getpeername(struct thread *td, int fd, struct sockaddr **sa,
    socklen_t *alen);
#endif /* __rtems__ */
/*
 * getpeername1() - Get name of peer for connected socket.
 */
static int
getpeername1(struct thread *td, struct getpeername_args *uap, int compat)
{
	struct sockaddr *sa;
	socklen_t len;
	int error;

	error = copyin(uap->alen, &len, sizeof (len));
	if (error != 0)
		return (error);

	error = kern_getpeername(td, uap->fdes, &sa, &len);
	if (error != 0)
		return (error);

	if (len != 0) {
#ifdef COMPAT_OLDSOCK
		if (compat && SV_PROC_FLAG(td->td_proc, SV_AOUT))
			((struct osockaddr *)sa)->sa_family = sa->sa_family;
#endif
		error = copyout(sa, uap->asa, (u_int)len);
	}
	free(sa, M_SONAME);
	if (error == 0)
		error = copyout(&len, uap->alen, sizeof(len));
	return (error);
}
#ifdef __rtems__
int
getpeername(int socket, struct sockaddr *__restrict address,
    socklen_t *__restrict address_len)
{
	struct thread *td = rtems_bsd_get_curthread_or_null();
	struct getpeername_args ua = {
		.fdes = socket,
		.asa = address,
		.alen = address_len
	};
	int error;

	if (td != NULL) {
		error = getpeername1(td, &ua, 0);
	} else {
		error = ENOMEM;
	}

	return rtems_bsd_error_to_status_and_errno(error);
}
#endif /* __rtems__ */

int
kern_getpeername(struct thread *td, int fd, struct sockaddr **sa,
    socklen_t *alen)
{
	struct socket *so;
	struct file *fp;
	socklen_t len;
	int error;

	AUDIT_ARG_FD(fd);
	error = getsock_cap(td, fd, &cap_getpeername_rights,
	    &fp, NULL, NULL);
	if (error != 0)
		return (error);
	so = fp->f_data;
	if ((so->so_state & (SS_ISCONNECTED|SS_ISCONFIRMING)) == 0) {
		error = ENOTCONN;
		goto done;
	}
	*sa = NULL;
	CURVNET_SET(so->so_vnet);
	error = (*so->so_proto->pr_usrreqs->pru_peeraddr)(so, sa);
	CURVNET_RESTORE();
	if (error != 0)
		goto bad;
	if (*sa == NULL)
		len = 0;
	else
		len = MIN(*alen, (*sa)->sa_len);
	*alen = len;
#ifdef KTRACE
	if (KTRPOINT(td, KTR_STRUCT))
		ktrsockaddr(*sa);
#endif
bad:
	if (error != 0 && *sa != NULL) {
		free(*sa, M_SONAME);
		*sa = NULL;
	}
done:
	fdrop(fp, td);
	return (error);
}

#ifndef __rtems__
int
sys_getpeername(struct thread *td, struct getpeername_args *uap)
{

	return (getpeername1(td, uap, 0));
}

#ifdef COMPAT_OLDSOCK
int
ogetpeername(struct thread *td, struct ogetpeername_args *uap)
{

	/* XXX uap should have type `getpeername_args *' to begin with. */
	return (getpeername1(td, (struct getpeername_args *)uap, 1));
}
#endif /* COMPAT_OLDSOCK */
#endif /* __rtems__ */

static int
sockargs(struct mbuf **mp, char *buf, socklen_t buflen, int type)
{
	struct sockaddr *sa;
	struct mbuf *m;
	int error;

	if (buflen > MLEN) {
#ifdef COMPAT_OLDSOCK
		if (type == MT_SONAME && buflen <= 112 &&
		    SV_CURPROC_FLAG(SV_AOUT))
			buflen = MLEN;		/* unix domain compat. hack */
		else
#endif
			if (buflen > MCLBYTES)
				return (EINVAL);
	}
	m = m_get2(buflen, M_WAITOK, type, 0);
	m->m_len = buflen;
	error = copyin(buf, mtod(m, void *), buflen);
	if (error != 0)
		(void) m_free(m);
	else {
		*mp = m;
		if (type == MT_SONAME) {
			sa = mtod(m, struct sockaddr *);

#if defined(COMPAT_OLDSOCK) && BYTE_ORDER != BIG_ENDIAN
			if (sa->sa_family == 0 && sa->sa_len < AF_MAX &&
			    SV_CURPROC_FLAG(SV_AOUT))
				sa->sa_family = sa->sa_len;
#endif
			sa->sa_len = buflen;
		}
	}
	return (error);
}

int
#ifndef __rtems__
getsockaddr(struct sockaddr **namp, caddr_t uaddr, size_t len)
#else /* __rtems__ */
getsockaddr(struct sockaddr **namp, const struct sockaddr *uaddr, size_t len)
#endif /* __rtems__ */
{
	struct sockaddr *sa;
#ifndef __rtems__
	int error;
#endif /* __rtems__ */

	if (len > SOCK_MAXADDRLEN)
		return (ENAMETOOLONG);
	if (len < offsetof(struct sockaddr, sa_data[0]))
		return (EINVAL);
#ifndef __rtems__
	sa = malloc(len, M_SONAME, M_WAITOK);
	error = copyin(uaddr, sa, len);
	if (error != 0) {
		free(sa, M_SONAME);
	} else {
#if defined(COMPAT_OLDSOCK) && BYTE_ORDER != BIG_ENDIAN
		if (sa->sa_family == 0 && sa->sa_len < AF_MAX &&
		    SV_CURPROC_FLAG(SV_AOUT))
			sa->sa_family = sa->sa_len;
#endif
		sa->sa_len = len;
		*namp = sa;
	}
	return (error);
#else /* __rtems__ */
	sa = memcpy(*namp, uaddr, len);
	sa->sa_len = len;
	return (0);
#endif /* __rtems__ */
}

/*
 * Dispose of externalized rights from an SCM_RIGHTS message.  This function
 * should be used in error or truncation cases to avoid leaking file descriptors
 * into the recipient's (the current thread's) table.
 */
void
m_dispose_extcontrolm(struct mbuf *m)
{
#ifndef __rtems__
	struct cmsghdr *cm;
	struct file *fp;
	struct thread *td;
	socklen_t clen, datalen;
	int error, fd, *fds, nfd;

	td = curthread;
	for (; m != NULL; m = m->m_next) {
		if (m->m_type != MT_EXTCONTROL)
			continue;
		cm = mtod(m, struct cmsghdr *);
		clen = m->m_len;
		while (clen > 0) {
			if (clen < sizeof(*cm))
				panic("%s: truncated mbuf %p", __func__, m);
			datalen = CMSG_SPACE(cm->cmsg_len - CMSG_SPACE(0));
			if (clen < datalen)
				panic("%s: truncated mbuf %p", __func__, m);

			if (cm->cmsg_level == SOL_SOCKET &&
			    cm->cmsg_type == SCM_RIGHTS) {
				fds = (int *)CMSG_DATA(cm);
				nfd = (cm->cmsg_len - CMSG_SPACE(0)) /
				    sizeof(int);

				while (nfd-- > 0) {
					fd = *fds++;
					error = fget(td, fd, &cap_no_rights,
					    &fp);
					if (error == 0) {
						fdclose(td, fp, fd);
						fdrop(fp, td);
					}
				}
			}
			clen -= datalen;
			cm = (struct cmsghdr *)((uint8_t *)cm + datalen);
		}
		m_chtype(m, MT_CONTROL);
	}
#endif /* __rtems__ */
}
