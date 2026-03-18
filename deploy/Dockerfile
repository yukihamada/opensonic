FROM nginx:alpine

COPY web/ /usr/share/nginx/html/

COPY security-headers.conf /etc/nginx/snippets/security-headers.conf
COPY nginx.conf /etc/nginx/conf.d/default.conf

EXPOSE 8080
